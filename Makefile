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
WARN     ?= -Wall -Wextra -pedantic
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

.PHONY: all clean install uninstall debug release docs lint format format-check \
        test bench modules help check

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

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)

# ── docs / tidy ──────────────────────────────────────────────────────────────
docs:
	@command -v doxygen >/dev/null || { echo "doxygen not installed"; exit 1; }
	doxygen Doxyfile

lint:
	@command -v cppcheck >/dev/null && cppcheck --enable=all --quiet \
	    --suppress=missingIncludeSystem $(INCS) $(SRC_DIR)/ \
	    || echo "(cppcheck missing — apt install cppcheck)"

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

check: lint
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
