# ZT-006: `filter_stop()` blocking `waitpid` hangs the UI when the filter ignores SIGTERM

- **Severity:** 🟠 medium (a misbehaving `--filter` child freezes the whole
  interactive terminal indefinitely; no input, no render, no recovery)
- **Area:** ext (external filter subprocess)
- **Status:** ✅ resolved — fixed 2026-06-03 (branch `fix/zt-001-ownership-and-ui-hangs`). `filter_stop()` now uses a bounded `WNOHANG` grace window + `SIGKILL` escalation; no blocking `waitpid` on the loop tick.
- **Location:** `src/ext/filter.c:86`

## Root cause

`filter_stop()` terminates the filter child by sending `SIGTERM` and then
*blocking* on `waitpid` until the child is reaped:

```c
kill(c->ext.filter_pid, SIGTERM);
int status = 0;
waitpid(c->ext.filter_pid, &status, 0);   /* flags == 0: blocks forever */
```
(`src/ext/filter.c:84`)

The `0` flags mean `waitpid` will not return until the child actually exits.
`SIGTERM` is catchable and ignorable, so any filter command that traps or
ignores it — or is itself blocked in an uninterruptible state — leaves
`filter_stop()` parked inside the kernel with no timeout and no escalation to
`SIGKILL`.

This matters because `filter_stop()` is not reserved for clean shutdown — it is
called from the *normal event-loop tick* on routine I/O conditions:

- `filter_feed()` calls `filter_stop(c)` when the write to the child's stdin
  fails with anything other than `EAGAIN`/`EINTR` (`src/ext/filter.c:108`).
- `filter_drain()` calls `filter_stop(c)` on EOF (`read` returns 0) from the
  child's stdout (`src/ext/filter.c:128`).

Both `filter_feed()` and `filter_drain()` run synchronously inside the
single-threaded loop (`filter_drain()` is invoked off the loop's poll of
`filter_poll_fd()`; `filter_feed()` is invoked from `rx_ingest()` →
`render.c:349`). A child that has closed its stdout (so `filter_drain` reaches
EOF and calls `filter_stop`) but has *not* exited — e.g. it forked a grandchild,
or it is wedged ignoring SIGTERM — will park the entire UI on the blocking
`waitpid`. The terminal stops responding to keystrokes and stops repainting
until the child finally dies, which may be never.

This is a direct violation of the "never block in the single-threaded event
loop" rule.

## Trigger / repro

1. Start with a filter that ignores SIGTERM and stops reading, e.g.
   `zyterm /dev/ttyUSB0 --filter 'trap "" TERM; head -c0; sleep 100000'`
   (or any command that closes stdout early yet stays alive ignoring TERM).
2. Drive enough RX through the device that `filter_drain()` hits EOF, or trigger
   a stdin write error in `filter_feed()`.
3. `filter_stop()` runs, sends an ignored `SIGTERM`, then blocks in `waitpid`.
4. Observe: the zyterm UI freezes — no key handling, no HUD refresh — until the
   child eventually exits (here, after `sleep 100000`).

## Fix direction

Make reaping non-blocking with bounded escalation:

- Replace the blocking `waitpid(pid, &status, 0)` with `WNOHANG`. If the child
  has not exited, record the pending pid and let the main loop's reaper retry on
  subsequent ticks (mirroring how hooks are reaped).
- Escalate: send `SIGTERM`, and after a short grace period send `SIGKILL`, then
  reap with `WNOHANG`. Never let a single tick stall on a child's exit.
- Until reaped, keep the fds closed and `filter_pid` in a "terminating" state so
  the loop doesn't re-enter `filter_stop()` for the same child.

See theme (D) "blocking calls in the single-threaded loop"
(ZT-006/024/026) in
[`../../plans/RELIABILITY_HARDENING.md`](../../plans/RELIABILITY_HARDENING.md),
the loop-blocking rule in
[`../../invariants/INVARIANTS.md`](../../invariants/INVARIANTS.md) §3, and the
related EINTR byte-drop in `filter_feed` tracked as ZT-015 (board row in
[`../KNOWN_ISSUES.md`](../KNOWN_ISSUES.md)).

## Verify

- Integration: launch a filter that ignores SIGTERM, trigger `filter_stop()`,
  and assert the loop continues to service stdin/HUD within one tick (the child
  is escalated to SIGKILL and reaped without blocking).
- Manual: run the repro; confirm the UI stays responsive and the child is gone
  within the grace window.
