# ZT-005: failed `Ctrl+A A` autobaud strands `serial.fd = -1`, reconnect never fires

- **Severity:** 🟠 medium (device stays effectively dead while the HUD still
  reads "connected"; recovery needs a manual reconnect the operator has no
  reason to attempt)
- **Area:** loop / serial
- **Status:** ✅ resolved — fixed 2026-06-03 (branch `fix/zt-001-ownership-and-ui-hangs`). The `Ctrl+A A` failure path now recovers like `Ctrl+A r` instead of leaving `fd == -1`.
- **Location:** `src/loop/input.c:130` (the `Ctrl+A A` handler), with the
  consequence in `src/loop/runtime.c:118`

## Root cause

The command-mode `A` key closes the live serial fd *before* probing, then hands
off to `autobaud_probe()`:

```c
case 'A': {
    rx_thread_pause(c);
    if (c->serial.fd >= 0) {
        close(c->serial.fd);
        c->serial.fd = -1;          /* fd intentionally invalidated */
    }
    autobaud_probe(c);
    break;
}
```
(`src/loop/input.c:130`)

`autobaud_probe()` opens each candidate rate on a fresh fd and, on success,
assigns the winner to `c->serial.fd`. But on the failure path it returns `-1`
without ever reopening the port:

```c
if (best_fd < 0) {
    rx_thread_unpause(c);
    return -1;                      /* c->serial.fd is still -1 */
}
```
(`src/serial/autobaud.c:62`)

So if no probed rate scores any printable bytes (cable unplugged, peer silent,
wrong framing) the context is left with `c->serial.fd == -1` and no further
action.

The event loop builds its poll set unconditionally from that field:

```c
pfds[0].fd     = c->serial.fd;      /* == -1 after a failed autobaud */
pfds[0].events = threaded ? 0 : POLLIN;
```
(`src/loop/runtime.c:118`)

`poll(2)` *ignores* any pollfd whose `fd` is negative — it neither reports an
error nor sets any `revents` bit for that slot. The runtime's recovery paths all
hang off `pfds[0].revents` (`POLLERR | POLLNVAL` at `runtime.c:142`, `POLLHUP`
at `:154`), so none of them ever trigger. `run_reconnect_loop()` is therefore
never called, even though `c->core.reconnect` is on by default. The serial fd is
silently absent from the loop forever.

The HUD reads connection state from `c->serial.baud` / device name, not from the
fd, so it continues to render a connected-looking status line, hiding the dead
state from the operator.

## Trigger / repro

1. Start `zyterm /dev/ttyUSB0` against a port that is idle or whose peer emits no
   printable traffic.
2. Press `Ctrl+A` then `A` to run autobaud.
3. Every candidate rate scores 0.0; `autobaud_probe()` returns `-1`.
4. Observe: input is accepted but nothing is ever sent/received, the HUD still
   shows the old baud, and re-plugging the device does **not** auto-reconnect.
   Only a manual `Ctrl+A r` reopens the port.

## Fix direction

On the autobaud failure path, restore a usable fd instead of leaving `-1`:

- Have `autobaud_probe()` (or its caller in `input.c`) reopen the port at the
  prior `c->serial.baud` on failure, or
- Drive `run_reconnect_loop(c)` directly from the `A` handler when the probe
  returns `< 0`, so the standard reconnect machinery takes over.

Either way, the loop must never be left polling a permanently-negative fd while
`reconnect` is enabled. Consider also guarding the poll-set build in
`runtime.c:118` so a `serial.fd < 0` is forced into the reconnect path rather
than silently dropped — a defensive backstop independent of the autobaud caller.

See [`../KNOWN_ISSUES.md`](../KNOWN_ISSUES.md) for the board row, and theme (D)
"blocking/missing recovery in the single-threaded loop" in
[`../../plans/RELIABILITY_HARDENING.md`](../../plans/RELIABILITY_HARDENING.md).
The reconnect contract this violates is stated in
[`../../invariants/INVARIANTS.md`](../../invariants/INVARIANTS.md) §4 (reader
thread & fd lifecycle).

## Verify

- Unit/integration: simulate a failed probe (all rates score 0) and assert that
  after the handler returns, either `c->serial.fd >= 0` or the reconnect loop has
  been entered — never a quiescent `-1`.
- Manual: run the repro above; after a failed autobaud, unplug/replug the device
  and confirm the link comes back automatically with `--reconnect` on, and that
  the HUD reflects the disconnected state in the interim.
