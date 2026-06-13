# ADR-0004: Auto-reconnect on by default

- **Status:** accepted
- **Date:** 2026-06-03

## Context

Bench hardware is physically unreliable on purpose: you reset the board, you reflash it, you yank
the USB-serial adapter to move it, the device re-enumerates under a new `/dev/ttyUSB*` path. On a
hang-up (`POLLHUP`, read EOF), a naive terminal exits — and the operator loses their session,
their on-screen scrollback, and their place, for an event that is routine and self-healing.

The question is what the *default* behavior should be when the serial fd dies. Two defensible
positions:

- **Exit on hang-up** (classic terminal behavior): predictable, scriptable, surprising to nobody
  who has used `cu` or `screen`.
- **Reconnect on hang-up**: matches how the hardware actually behaves, keeps the session alive
  across the replug/reset that the operator *expected* to do.

zyterm's audience is doing iterative hardware work where replugging is the norm, not the
exception. Optimizing the default for the common case keeps people in flow.

## Decision

**Auto-reconnect defaults ON.** `c.core.reconnect` is initialized to `true` at
`src/main.c:340` (`/* auto-reconnect on hang-up by default */`). On a serial hang-up the runtime
re-opens the device — re-resolving the path when discovery hints (`--port-glob` /
`--match-vid-pid`) are in play, so a device that comes back under a different `/dev/ttyUSB*` is
still found.

Operators who want classic exit-on-hang-up behavior opt out with **`--no-reconnect`**
(`src/main.c:419`, handled at `src/main.c:506` → `c.core.reconnect = false`; `--reconnect` at
`:505` exists for explicitness and config overrides). `--no-reconnect` is documented as "exit on
serial hang-up" (`src/main.c:131`).

## Consequences

- The session survives resets, reflashes, and replugs without operator intervention — the common
  bench case is the smooth case.
- Reconnect is part of the loop's fd lifecycle, which means it interacts with the
  never-block-in-the-loop rule and with the threaded reader's fd ownership; that machinery is
  documented in [`design/THREADING_AND_RECONNECT.md`](../design/THREADING_AND_RECONNECT.md) and
  governed by **[INVARIANTS §4](../invariants/INVARIANTS.md)** (reader thread & fd lifecycle).
- Because reconnect re-resolves the device path, **pointer ownership of the device string matters**:
  freeing a non-heap path (e.g. the positional `argv[optind]`) on reconnect is a real failure mode —
  see ZT-002 (`src/serial/port_discover.c:171`) in
  [`tracking/KNOWN_ISSUES.md`](../tracking/KNOWN_ISSUES.md). The fix is a single-ownership rule for
  the device string, not a change to this default.
- Related defects in the reconnect path are tracked, not relitigated here: a failed `Ctrl+A A`
  autobaud that leaves the fd at `-1` so reconnect never fires (ZT-005), and a non-threaded
  `POLLHUP` path that reads once and can lose buffered RX before reconnecting (ZT-027).
