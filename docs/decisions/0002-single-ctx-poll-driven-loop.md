# ADR-0002: One `zt_ctx`, single-threaded poll-driven loop

- **Status:** accepted
- **Date:** 2026-06-03

## Context

A serial terminal multiplexes a handful of byte sources — the serial fd, stdin, and (optionally)
HTTP/SSE/WebSocket clients, a metrics socket, a detach socket, child-process filters — onto one
operator terminal. The natural failure mode of such a tool is a UI that stutters or hangs because
some I/O blocked.

We had two axes of choice:

1. **State organization.** Spread per-feature state across module-private globals, or gather it
   into one place.
2. **Concurrency model.** A thread per source (simple to write, hard to reason about — locks,
   ordering, signal interactions), or a single event loop (one mental model, but every call in it
   must be non-blocking).

For a ~12.5K-LOC C program maintained by a small team, correctness and debuggability dominate.
Threads buy parallelism we mostly do not need: a human types slowly and a terminal repaints at
human cadence. The one place threads genuinely help is draining a fast serial device at high baud,
where the cost of *not* reading promptly is dropped bytes.

## Decision

- **One `zt_ctx`** (`src/zt_ctx.h`) holds all per-process runtime state — fds, buffers, render
  flags, scrollback ring, mode toggles — grouped into feature tiers. Modules operate on a single
  instance passed by pointer. This avoids circular includes and keeps "what is the program's
  state?" answerable by reading one struct.
- **A single-threaded `poll(2)` event loop** (`src/loop/runtime.c`, e.g. the main wait at
  `runtime.c:132`) drives everything. Every call made from the loop must be non-blocking; nothing
  in the loop may sit in a blocking syscall.
- **High-baud reads are the one sanctioned exception, and it is opt-in.** `--threaded`
  (`src/main.c:443`) starts a single reader thread (`src/loop/rx_thread.c`) that owns *only* a
  private `dup()` of the serial fd and a **1 MiB lock-free SPSC ring** (`ZT_SPSC_CAP`,
  `src/zt_ctx.h:69`) published with release/acquire atomics. The main loop is the sole consumer;
  the reader is the sole producer. No shared mutable state, no locks.
- **Signal handlers touch only `sig_atomic_t` flags.** `zt_g_quit` and `zt_g_winch`
  (`src/core/core.c:39-40`) are the only state a handler writes; the loop polls them. Handlers do
  no allocation, no I/O, no locking.

## Consequences

- One concurrency model to hold in your head. Bugs are sequential bugs.
- **The cardinal rule is "never block in the loop."** Any blocking call reachable from a loop tick
  is a defect — and several audit findings are exactly that: a blocking `waitpid` in `filter_stop`
  (ZT-006), a blocking metrics client fd (ZT-024), an unbounded TX `poll()` retry (ZT-026). These
  are tracked in [`tracking/KNOWN_ISSUES.md`](../tracking/KNOWN_ISSUES.md).
- The opt-in reader thread has a narrow, well-defined contract that must not erode: private fd,
  SPSC ring, no locks, no shared writes.
- This decision produces three invariants — see
  **[INVARIANTS §3](../invariants/INVARIANTS.md)** (the single-threaded event loop — never block
  in it), **[INVARIANTS §4](../invariants/INVARIANTS.md)** (reader thread & fd lifecycle — the
  SPSC ring), and **[INVARIANTS §8](../invariants/INVARIANTS.md)** (module dependency chain &
  `zt_ctx` ownership). See also **[INVARIANTS §2](../invariants/INVARIANTS.md)** for the
  signal/async-signal-safety rules.
- Design detail for the reader and reconnect machinery lives in
  [`design/THREADING_AND_RECONNECT.md`](../design/THREADING_AND_RECONNECT.md).
