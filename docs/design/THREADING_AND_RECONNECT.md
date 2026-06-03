# Design: the reader thread and reconnect / hot-plug

How zyterm's optional reader thread and its reconnect / hot-plug model were designed,
and why. Rationale layer — the line-by-line walk-through is in
[../reference/INTERNALS.md](../reference/INTERNALS.md); the don't-regress rules are
[../invariants/INVARIANTS.md](../invariants/INVARIANTS.md) §4 (reader thread & fd
lifecycle) and §3 (never block the loop); the on-by-default reconnect decision is
[../decisions/0004-reconnect-on-by-default.md](../decisions/0004-reconnect-on-by-default.md).

Source of truth: `src/loop/rx_thread.c`, `src/ext/reconnect.c`.

## Why the reader thread is opt-in

zyterm's default runtime is a single `poll(2)`-driven loop
([../decisions/0002-single-ctx-poll-driven-loop.md](../decisions/0002-single-ctx-poll-driven-loop.md)):
one thread services stdin, the serial fd, and the HTTP fds together. That is the right
default — it is simple, has no synchronization, and at typical baud rates the loop keeps
up trivially.

The problem it does *not* solve is **jitter at high baud**. When the render/log pipeline
takes a moment (a large scrollback redraw, a slow terminal), the serial fd isn't being
`read()` during that window, the kernel UART buffer can fill, and bytes back up. The
optional `--threaded` reader (`src/loop/rx_thread.c`) decouples UART-interrupt latency
from render latency: a dedicated thread drains the device into a ring as fast as the
kernel delivers, and the main thread consumes from the ring at its own pace.

It is **opt-in, not default**, because at low baud it is a strict pessimization — an
extra thread, an extra syscall, and a wake per chunk buy nothing when the loop was never
behind (`src/loop/rx_thread.c:9-12`). So the cost is only paid by users who ask for it.

## The concurrency model: private dup fd + lock-free SPSC ring + wake pipe

Three pieces make the threaded path safe (`src/loop/rx_thread.c:14-24`):

**A private `dup()` of the serial fd.** The worker does *not* read `c->serial.fd`
directly. On start it takes `fcntl(c->serial.fd, F_DUPFD_CLOEXEC, 3)` and owns that
descriptor in `r->local_fd` (`:135-144`). This is the key ownership decision: because
the worker reads its own private fd, the main thread is free to `close()` and reopen
`c->serial.fd` — for autobaud or reconnect — and even let the OS recycle the fd *number*,
without ever racing the worker's in-flight `read()`. The worker keeps reading the old
device until it is explicitly told to stop. This ownership split is recorded in
[INVARIANTS §4](../invariants/INVARIANTS.md).

**A lock-free SPSC ring.** A single-producer/single-consumer ring of `ZT_SPSC_CAP` =
1 MiB (`src/zt_ctx.h:69`), power-of-two so the index wrap is a mask
(`src/loop/rx_thread.c:87`). The worker is the only producer (`rx_thread_main`,
`:65`); the main thread is the only consumer (`rx_thread_drain`, `:208`). Coordination
is `release`/`acquire` atomics on `head` and `tail` only — no mutex. The producer
publishes bytes with a `release` store to `head` after writing them; the consumer reads
`head` with an `acquire` load, guaranteeing it sees the bytes the store published. The
drain copies out in up to two contiguous spans to avoid a byte-by-byte loop across the
wrap (`:217-222`).

**A wake pipe.** A non-blocking `pipe2(O_CLOEXEC | O_NONBLOCK)` (`:146`) lets the worker
nudge the main loop's `poll()` when it has enqueued data (`wake_main()`, `:58`). The
main thread polls the read end alongside stdin and the HTTP fds; on drain it empties any
pending wake bytes so `poll()` doesn't spin (`:224-228`). The write is best-effort: if
the pipe is full the worker doesn't care, because there is already a pending wake.

**Shutdown is race-free by construction** (`rx_thread_stop`, `:174`): main flips
`running` to 0 (release), then `atomic_exchange`s `local_fd` to -1 and `close()`s the old
dup. Closing the dup makes any blocked `read()` in the worker return `EBADF`, which the
worker treats as a clean exit (`:97`); main then `pthread_join`s. Without the close, the
worker could sleep up to 50 ms on an error backoff before noticing the stop flag.

### What the reader thread does *not* touch

The worker's entire world is its private dup, the ring, the wake pipe, and the
`running`/`local_fd` atomics. It never calls into the render, log, framing, or HTTP code
— all of that runs on the main thread against main-thread-owned state. This is why the
framing decoders can keep their state on `c->proto` without locking (see
[FRAMING_AND_CRC.md](FRAMING_AND_CRC.md)). The boundary is enforced in
[INVARIANTS §4](../invariants/INVARIANTS.md).

### Start / stop are driven by user intent, not thread state

There are two distinct booleans, and conflating them is a bug:

- `spsc_enabled` — *user intent*, set once by the CLI parser when `--threaded` is given.
- `spsc_impl` — the *authoritative "thread is up" marker*; non-NULL iff the thread is
  running (`:117-120`, `:238-240`).

`rx_thread_pause()` / `rx_thread_unpause()` (`:252-258`) bracket every `c->serial.fd`
swap (autobaud, reconnect, manual reopen). Pause is a no-op when already stopped;
unpause only restarts if `spsc_enabled` is set. Critically, `rx_thread_stop()` must
**not** clear `spsc_enabled` (`:200-202`) — if it did, a paired suspend/resume around a
reconnect would silently disable threading for the rest of the session. Pause/unpause are
idempotent so nested or repeated calls are safe.

## The reconnect / hot-plug model: close → reopen, pause/unpause, re-discovery

Reconnect is on by default ([ADR-0004](../decisions/0004-reconnect-on-by-default.md);
disable with `--no-reconnect`) because a serial workflow is mostly *waiting for a device
to show up or come back* — an MCU resetting, a USB adapter being replugged. The model is
deliberately simple: on link loss, close the fd and sit in a tight wait-and-retry loop
until the device reopens, keeping the UI fully responsive throughout.

`run_reconnect_loop()` (`src/ext/reconnect.c:107`) is the heart of it. On entry it:

1. **Pauses the RX worker** (`rx_thread_pause(c)`, `:110`). This is mandatory: the
   worker's dup of the now-dead fd would otherwise keep returning `EIO` and spinning.
2. **Closes `c->serial.fd`** and sets it to -1 (`:111-114`), then fires the
   `disconnect` hook (`:115`) — env-driven user hooks, see
   [../guide/automation.md](../guide/automation.md).
3. Sets `c->tui.disconnected`, drops the centered modal popup, and shows the persistent
   "◆ DISCONNECTED" HUD pill.

It then loops at `ZT_RECONNECT_MS` = 1000 ms granularity (`src/zt_ctx.h:65`),
`poll()`ing stdin so the user keeps full control while disconnected: scrollback,
in-app selection + OSC 52 copy, search, `Ctrl+A r` to force a retry, `Ctrl+A x`/`q` to
quit. All other command-mode keys are gated off by `c->tui.disconnected` so a stray
keystroke can't, say, spawn a log file with no device attached (`:91-106`).

Each tick calls `reconnect_attempt()` (`:40`):

- **Port re-discovery.** If `--port-glob` or `--match-vid-pid` were given, it calls
  `port_rediscover(c)` *every attempt* (`:45`). A USB-serial adapter that comes back as a
  different `/dev/ttyUSBn` after replug is transparently re-resolved; without those hints
  the device path is held fixed. See [../reference/CLI.md](../reference/CLI.md) for the
  discovery flags.
- **Reopen.** `try_reopen_serial()` with the saved line settings; on success it installs
  the new fd and returns 0 (`:47-51`).

On success the loop **unpauses the RX worker** (`rx_thread_unpause`, `:207`) — which dups
the *new* fd — clears the disconnected state, flashes "✓ reconnected", and fires the
`connect` hook (`:204-217`). Notably it does **not** yank the view: if the user was
scrolled back reading history, it leaves them there and lets them `PgDn` to live when
ready.

The same pause → close → reopen → unpause discipline is reused by the manual `Ctrl+A r`
reconnect and by autobaud, so there is exactly one fd-swap protocol in the codebase.

## Open items and defects

- **Backpressure / byte-drop is an open design item.** When the SPSC ring is full the
  worker writes only what fits and drops the rest: `free_sp = r->cap - (head - tail)` and
  `wr = min(n, free_sp)` (`src/loop/rx_thread.c:84-85`) — surplus bytes from that
  `read()` are silently discarded. With a 1 MiB ring this only bites if the main thread
  stalls for a long time at high baud, but it is a *silent* loss with no counter or flash.
  The intended fix (backpressure or at minimum a drop counter surfaced in the HUD) is
  tracked in [../plans/RELIABILITY_HARDENING.md](../plans/RELIABILITY_HARDENING.md).

- **ZT-002** (🔴 high, `src/serial/port_discover.c:171`) —
  [issue](../tracking/issues/ZT-002-port-rediscover-frees-argv-device.md). The
  re-discovery path that reconnect calls every attempt can `free()` a device string that
  is actually the non-heap `argv[optind]` when `--port-glob` plus a positional device
  resolve to a different path on the first reconnect → heap corruption / abort. This is
  one face of the mixed pointer-ownership theme; the fix is single ownership (always
  `strdup` the device so it is heap-owned). The startup-time sibling is **ZT-001**
  (`src/ext/profile.c:93`,
  [issue](../tracking/issues/ZT-001-profile-load-frees-argv-device.md)). See
  [INVARIANTS §1](../invariants/INVARIANTS.md).

- **ZT-005** (🟠 medium, `src/loop/input.c:130`) —
  [issue](../tracking/issues/ZT-005-autobaud-strands-fd.md). A failed `Ctrl+A A` autobaud
  can leave `serial.fd = -1`; because `poll()` ignores a negative fd, the reconnect loop
  never fires and the HUD still shows connected. Fix: drive the reconnect/reopen path on
  autobaud failure. This is relevant here because autobaud shares the pause/unpause fd-swap
  protocol described above.

- **ZT-027** (⚪ low, `src/loop/runtime.c:156`) — in the *non-threaded* path, the
  `POLLHUP` branch reads once and can lose buffered RX before reconnect; fix is to drain
  in a loop like the `POLLIN` path. (The threaded path drains the ring separately.)

---

_Last updated: 2026-06-03._
