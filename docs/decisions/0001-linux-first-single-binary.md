# ADR-0001: Linux-first, single self-contained binary

- **Status:** accepted
- **Date:** 2026-06-03

## Context

zyterm is a serial-terminal emulator for hardware bring-up: someone at a bench with a USB-serial
adapter, a `/dev/ttyUSB0`, and a need to see bytes immediately and reliably. That environment is
overwhelmingly Linux. Supporting several operating systems as first-class targets would multiply
the surface we must test against — termios variants, baud-rate ioctls, clipboard backends, event
mechanisms — for users we do not have.

We also want the tool to be trivial to deploy: copy one file to a lab machine and run it, with no
runtime package to install and no shared-library version to chase. A serial console is exactly the
kind of thing you reach for on a freshly imaged or minimal system.

Two constraints follow from "one file, no extra packages":

- The runtime dependency set must be **libc only**. Anything heavier (a curses library, an X11
  client library, a TLS stack) would reintroduce the deployment problem we are trying to avoid.
- The binary must tolerate being built on a newer toolchain than the machine it runs on. Modern
  glibc redirects `strtol`/`strtoul` to versioned `__isoc23_*` symbols, which fail to resolve on
  older runtime glibc.

## Decision

**Linux is the supported and CI-tested platform.** zyterm ships as **one self-contained C
binary** (`./zyterm`) with a **libc-only runtime dependency**.

To honor that:

- **X11 clipboard is loaded at runtime, not linked.** The xcb backend is reached through
  `dlopen("libxcb.so.1")` and `dlsym()` at first use (`src/proto/clipboard.c:213`), so there is
  **no build-time dependency** on libxcb and no hard runtime dependency — if libxcb is absent, the
  clipboard simply falls back (OSC 52 still works).
- **Symbol versions are pinned for portability.** The release build pins `strtol`/`strtoul` to a
  base glibc symbol via `.symver` in `src/zt_internal.h:80-81`
  (`__asm__(".symver __isoc23_strtol,strtol@" ZT_GLIBC_BASE_VER)`), so a binary built on a new
  toolchain still loads on an older glibc. The pin is **skipped under sanitizers**
  (`ZT_SANITIZER_ACTIVE`, `src/zt_internal.h:53-60`) because it interferes with their interceptors.

zyterm **may still build on other Unix-likes** — the code carries non-Linux fallbacks (for
example the `termios2`/`BOTHER` block in `src/zt_ctx.h:29-52` is Linux-only, with an
`__APPLE__` `IOSSIOSPEED` path). Those builds are **unsupported**: not in CI, not release-tested,
and may lag or break without notice.

## Consequences

- Single-file deployment: `scp zyterm host:` and run. No package manager, no `.so` version dance.
- Reduced test matrix: CI exercises Linux × {gcc, clang} × {none, asan+ubsan} only
  (see [`ops/RELEASE.md`](../ops/RELEASE.md) and [`reference/ARCHITECTURE.md`](../reference/ARCHITECTURE.md)).
- Linux-only kernel features become fair game — `epoll`, `splice`, `termios2`, `inotify`,
  abstract/UNIX sockets — without a portability tax. (Whether we *use* one is a separate
  decision; see [ADR-0003](0003-epoll-splice-fastpath-deferred.md).)
- The `.symver` pin is build-arch-aware and must stay in step with the toolchain; it is part of
  the build-and-release surface. See **[INVARIANTS §9](../invariants/INVARIANTS.md)** (Build &
  release) for the don't-regress rules around portability, the symbol pin, and the sanitizer
  carve-out.
- The release build currently lacks hardening flags and uses `-march=native`; that is tracked as
  a fix in [`plans/RELIABILITY_HARDENING.md`](../plans/RELIABILITY_HARDENING.md), not relitigated
  here.
