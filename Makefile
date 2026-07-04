# ─────────────────────────────────────────────────────────────────────────────
# zyterm — serial terminal for embedded development
#
# Source layout (modules under src/):
#   core/    helpers, signals, terminal, time, output buffer, CRC
#   serial/  serial port setup, fast I/O, kernel UART counters, autobaud
#   log/     persistent log file, JSONL emit, scrollback ring
#   proto/   frame decoders, X/Y/ZMODEM, F-key macros, clipboard (dlopen xcb),
#            OSC, SGR, KGDB pass-through
#   render/  RX rendering pipeline, throughput sparkline
#   tui/     HUD, dialogs, search, pager, fuzzy finder
#   net/     HTTP/SSE/WS bridge, Prometheus metrics, detach/attach sessions
#   ext/     bookmarks, diff, filter, log-level mute, multi-pane, profiles, reconnect
#   loop/    keyboard input, send pipeline, RX reader thread, run loops
#   main.c   CLI parsing + entry point
#
# Each src/.../<file>.c → build/obj/.../<file>.o → linked into ./zyterm.
# Requires: cc + make. Runtime: libc (glibc >= 2.34 on Linux).
# ─────────────────────────────────────────────────────────────────────────────

CC       ?= cc
CSTD     ?= -std=gnu11
OPT      ?= -O3
# -Werror=implicit-function-declaration turns the module layering into a
# compiler-enforced invariant: every non-main .c includes only its own narrow
# module header (core←serial←…←loop), so a call UP the chain has no declaration
# in scope and fails to compile instead of silently linking (INVARIANTS §8).
WARN     ?= -Wall -Wextra -pedantic -Werror=implicit-function-declaration
INCS     ?= -Iinclude -Isrc
CFLAGS   ?= $(OPT) $(WARN) $(CSTD) -D_GNU_SOURCE $(INCS)
LDFLAGS  ?=

# OS-aware link flags:
#   Linux : -lpthread -ldl  (glibc < 2.34 ships these as separate .so;
#                             glibc >= 2.34 absorbs them into libc but
#                             the flags are harmless stubs.)
#   macOS : -lpthread       (dlopen lives in libSystem; -ldl doesn't exist.)
#   FreeBSD: -lpthread      (dlopen is in libc.)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
LDLIBS   ?= -lpthread -ldl
else
LDLIBS   ?= -lpthread
endif

BIN       = zyterm
SRC_DIR   = src
OBJ_DIR   = build/obj

# Native X11 clipboard: loaded at runtime via dlopen("libxcb.so.1").
# No compile-time headers or pkg-config probe needed. Works on any
# graphical Linux desktop (X11 or XWayland). Falls back gracefully
# on headless / pure-Wayland / macOS where libxcb isn't present.

# Recursively gather all .c files under src/ (semantic modules + main.c).
SOURCES  := $(shell find $(SRC_DIR) -type f -name '*.c' | sort)
OBJECTS  := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SOURCES))
HEADERS  := $(shell find include $(SRC_DIR) -type f -name '*.h' 2>/dev/null)

# Subdirectories that need to exist under $(OBJ_DIR) before compilation.
OBJ_SUBDIRS := $(sort $(dir $(OBJECTS)))

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
MANDIR   ?= $(PREFIX)/share/man
BASHCOMPDIR ?= $(PREFIX)/share/bash-completion/completions
ZSHCOMPDIR  ?= $(PREFIX)/share/zsh/site-functions
FISHCOMPDIR ?= $(PREFIX)/share/fish/vendor_completions.d

.PHONY: all clean install uninstall debug release docs lint format format-check \
        test bench modules help check fuzz coverage layering-check

# ── primary targets ──────────────────────────────────────────────────────────
all: $(BIN)

$(BIN): $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS) | $(OBJ_SUBDIRS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_SUBDIRS):
	@mkdir -p $@

# ── convenience ──────────────────────────────────────────────────────────────
debug: CFLAGS := -O0 -g3 $(WARN) $(CSTD) -D_GNU_SOURCE $(INCS) -DZT_DEBUG=1
debug: clean all

release: CFLAGS += -flto -march=native -DNDEBUG
release: clean all

# Embeddable archive (drop-in to a host app). Excludes main.o.
zyterm_embed.a: $(filter-out $(OBJ_DIR)/main.o,$(OBJECTS))
	$(AR) rcs $@ $^

# ── install ──────────────────────────────────────────────────────────────────
install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -Dm644 docs/zyterm.1 $(DESTDIR)$(MANDIR)/man1/zyterm.1
	install -Dm644 contrib/completions/zyterm.bash \
	    $(DESTDIR)$(BASHCOMPDIR)/zyterm
	install -Dm644 contrib/completions/_zyterm \
	    $(DESTDIR)$(ZSHCOMPDIR)/_zyterm
	install -Dm644 contrib/completions/zyterm.fish \
	    $(DESTDIR)$(FISHCOMPDIR)/zyterm.fish

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN) \
	      $(DESTDIR)$(MANDIR)/man1/zyterm.1 \
	      $(DESTDIR)$(BASHCOMPDIR)/zyterm \
	      $(DESTDIR)$(ZSHCOMPDIR)/_zyterm \
	      $(DESTDIR)$(FISHCOMPDIR)/zyterm.fish

# ── docs / tidy ──────────────────────────────────────────────────────────────
docs:
	@command -v doxygen >/dev/null || { echo "doxygen not installed"; exit 1; }
	doxygen Doxyfile

lint:
	@command -v cppcheck >/dev/null && cppcheck --enable=all --quiet \
	    --suppress=missingIncludeSystem $(INCS) $(SRC_DIR)/ \
	    || echo "(cppcheck missing — apt install cppcheck)"

# libFuzzer target for the frame decoders (the untrusted device-byte parsers).
# Requires clang. Builds every non-main source + the harness with
# fuzzer+ASan+UBSan instrumentation and runs for FUZZ_SECONDS (default 20).
FUZZ_SECONDS ?= 20
fuzz:
	@command -v clang >/dev/null || { echo "(clang required for libFuzzer)"; exit 1; }
	@mkdir -p build/fuzz/corpus
	clang -g -O1 -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
	    -std=gnu11 -D_GNU_SOURCE $(INCS) \
	    $(filter-out $(SRC_DIR)/main.c,$(SOURCES)) tests/fuzz/fuzz_framing.c \
	    -lpthread -ldl -o build/fuzz/fuzz_framing
	build/fuzz/fuzz_framing -max_total_time=$(FUZZ_SECONDS) -rss_limit_mb=2048 build/fuzz/corpus

# Coverage ratchet: rebuild the embed archive + tests under gcc --coverage, run
# the suite, and aggregate src/ line coverage via gcov (ships with gcc — no
# gcovr/lcov needed). Fails if coverage falls below COVERAGE_MIN so a change that
# guts a tested path can't slip through. Raise the floor as coverage grows; never
# lower it without justification. Leaves an instrumented build behind — a plain
# `make` rebuilds.
COVERAGE_MIN ?= 30
coverage:
	@command -v gcov >/dev/null || { echo "(gcov required — it ships with gcc)"; exit 1; }
	@$(MAKE) --no-print-directory clean
	@CFLAGS="-O0 -g --coverage $(CSTD) -D_GNU_SOURCE $(INCS)" LDFLAGS="--coverage" \
	    $(MAKE) --no-print-directory zyterm_embed.a
	@CFLAGS="-O0 -g --coverage $(CSTD) -D_GNU_SOURCE $(INCS) -I../include -I../src" \
	    LDFLAGS="--coverage" $(MAKE) --no-print-directory -C tests run >/dev/null
	@tot=0; cov=0; \
	for gcno in $$(find $(OBJ_DIR) -name '*.gcno'); do \
	    d=$$(dirname "$$gcno"); base=$$(basename "$$gcno" .gcno); \
	    rel=$${d#$(OBJ_DIR)/}; src="$(SRC_DIR)/$$rel/$$base.c"; \
	    [ -f "$$src" ] || continue; \
	    line=$$(gcov -n -o "$$d" "$$src" 2>/dev/null | grep -A1 -F "File '$$src'" | grep 'Lines executed'); \
	    pct=$$(echo "$$line" | sed -E 's/Lines executed:([0-9.]+)% of ([0-9]+)/\1/'); \
	    n=$$(echo "$$line" | sed -E 's/Lines executed:([0-9.]+)% of ([0-9]+)/\2/'); \
	    [ -z "$$n" ] && continue; \
	    c=$$(awk "BEGIN{printf \"%d\", ($$pct/100.0)*$$n + 0.5}"); \
	    tot=$$((tot+n)); cov=$$((cov+c)); \
	done; \
	[ "$$tot" -gt 0 ] || { echo "✖ no coverage data (gcov produced nothing)"; exit 1; }; \
	pctall=$$(awk "BEGIN{printf \"%.2f\", (100.0*$$cov/$$tot)}"); \
	echo "── src/ line coverage: $$pctall% ($$cov/$$tot lines) — floor $(COVERAGE_MIN)% ──"; \
	awk "BEGIN{exit !($$pctall >= $(COVERAGE_MIN))}" \
	    || { echo "✖ below the $(COVERAGE_MIN)% floor — add tests (or lower COVERAGE_MIN with justification)"; exit 1; }; \
	echo "✔ coverage gate passed"

# Module-layering guard (INVARIANTS §8): every non-main .c must include only its
# own narrow module header and must not reach up the chain via a bare `extern`.
# Together with -Werror=implicit-function-declaration this keeps the layering
# acyclic and compiler-checkable.
layering-check:
	@bad=0; \
	for f in $$(find $(SRC_DIR) -name '*.c' ! -name main.c); do \
	    if grep -q '#include "zt_internal.h"' "$$f"; then \
	        echo "✖ $$f includes the umbrella zt_internal.h — use its own module header"; bad=1; fi; \
	    if grep -qE '^[[:space:]]*extern[[:space:]].*\(' "$$f"; then \
	        echo "✖ $$f declares a bare extern function — route up-calls through a c->core sink"; bad=1; fi; \
	done; \
	if [ $$bad -eq 0 ]; then echo "✔ module layering: narrow includes, no up-layer externs"; \
	else echo "layering check failed (INVARIANTS §8)"; exit 1; fi

format:
	@command -v clang-format >/dev/null \
	    && find $(SRC_DIR) include -name '*.[ch]' -print0 | xargs -0 clang-format -i \
	    || echo "(clang-format missing)"

format-check:
	@command -v clang-format >/dev/null || { echo "(clang-format missing)"; exit 1; }
	@find $(SRC_DIR) include -name '*.[ch]' -print0 \
	    | xargs -0 clang-format --dry-run --Werror 2>&1 \
	    && echo "✔ All files correctly formatted" \
	    || { echo "✖ Formatting issues found — run: make format"; exit 1; }

clean:
	rm -rf build $(BIN) zyterm_embed.a docs/api docs/html docs/latex
	@$(MAKE) -C tests clean 2>/dev/null || true

# ── tests / bench ────────────────────────────────────────────────────────────
test: zyterm_embed.a
	$(MAKE) -C tests run

bench: $(BIN)
	@bash bench/throughput.sh 2>/dev/null || echo "(bench/throughput.sh missing)"

# ── developer aids ───────────────────────────────────────────────────────────
modules:
	@echo "── zyterm source modules ──────────────────────────────"; \
	for d in $$(find $(SRC_DIR) -mindepth 1 -maxdepth 1 -type d | sort); do \
	    n=$$(find $$d -name '*.c' | wc -l); \
	    loc=$$(cat $$d/*.c 2>/dev/null | wc -l); \
	    printf "  %-12s  %2d files  %5d LOC\n" "$$(basename $$d)/" "$$n" "$$loc"; \
	done; \
	printf "  %-12s  %2d files  %5d LOC\n" "main.c" 1 \
	    "$$(wc -l < $(SRC_DIR)/main.c)"

check: lint layering-check
	@$(MAKE) --no-print-directory all >/dev/null && echo "ok release build"

help:
	@echo "Targets:"; \
	echo "  make               build ./zyterm (release)"; \
	echo "  make debug         -O0 -g3 -DZT_DEBUG"; \
	echo "  make release       -O3 -flto -march=native"; \
	echo "  make zyterm_embed.a     archive for embedders"; \
	echo "  make test          run unit + pty test suites"; \
	echo "  make docs          generate doxygen html in docs/api/html/"; \
	echo "  make lint          cppcheck across src/"; \
	echo "  make format        clang-format src/ + include/"; \
	echo "  make format-check  dry-run format check (for CI)"; \
	echo "  make modules       show per-module LOC summary"; \
	echo "  make check         lint + release build"; \
	echo "  make install     copy ./zyterm to $(BINDIR)"
