# tests/Makefile — runs zyterm test suites.
#
# Layout:
#   unit/         per-module isolated tests
#   integration/  cross-layer scenarios (currently empty placeholder)
#   pty/          pseudo-terminal harness
#
# Built stand-alone or invoked from repo-root `make test`.
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11 -D_GNU_SOURCE
CFLAGS  += -I../include -I../src
LDFLAGS ?=
LDLIBS  ?= -lpthread -lutil

# Pick up the same optional X11 libs the main Makefile linked into
# zyterm_embed.a, otherwise the test binaries fail to resolve
# xcb_disconnect &c. when ZT_HAVE_X11 was active during the archive
# build.
HAVE_XCB := $(shell pkg-config --exists xcb xcb-xfixes 2>/dev/null && echo yes)
ifeq ($(HAVE_XCB),yes)
LDLIBS  += $(shell pkg-config --libs xcb xcb-xfixes)
endif

ROOT    := $(abspath ..)
LIB     := $(ROOT)/zyterm_embed.a
BUILD   := build

UNIT_SRCS        := $(wildcard unit/*.c)
INTEGRATION_SRCS := $(wildcard integration/*.c)
PTY_SRCS         := $(wildcard pty/*.c)

UNIT_BINS        := $(patsubst %.c,$(BUILD)/%,$(UNIT_SRCS))
INTEGRATION_BINS := $(patsubst %.c,$(BUILD)/%,$(INTEGRATION_SRCS))
PTY_BINS         := $(patsubst %.c,$(BUILD)/%,$(PTY_SRCS))

ALL_BINS := $(UNIT_BINS) $(INTEGRATION_BINS) $(PTY_BINS)

.PHONY: all run unit integration pty clean

all: $(ALL_BINS)

$(LIB):
	$(MAKE) -C $(ROOT) zyterm_embed.a

$(BUILD)/%: %.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB) $(LDLIBS)

run: all
	@fail=0; \
	for b in $(ALL_BINS); do \
	    printf "── %s ─────────────────────────────\n" "$$b"; \
	    if ! "$$b"; then fail=1; fi; \
	done; \
	exit $$fail

unit:        $(UNIT_BINS);        @for b in $(UNIT_BINS);        do "$$b" || exit 1; done
integration: $(INTEGRATION_BINS); @for b in $(INTEGRATION_BINS); do "$$b" || exit 1; done
pty:         $(PTY_BINS);         @for b in $(PTY_BINS);         do "$$b" || exit 1; done

clean:
	rm -rf $(BUILD)
