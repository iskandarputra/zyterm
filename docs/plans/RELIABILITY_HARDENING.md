# Reliability hardening plan

> **Status (2026-06-13): COMPLETE.** All 28 audited defects (ZT-001 … ZT-028) named in the phases
> below have shipped on branch `fix/zt-001-ownership-and-ui-hangs`. The authoritative record is now
> the Resolved table in [tracking/KNOWN_ISSUES.md](../tracking/KNOWN_ISSUES.md) and the enforced
> rules in [invariants/INVARIANTS.md](../invariants/INVARIANTS.md); the phases below are kept as the
> historical plan-of-attack that was executed.

This was the prioritized fix-and-harden plan for zyterm 1.2.0 — what to attack, in what order, and
how to verify each step. Every item references the audit defect IDs (`ZT-NNN`) from
[tracking/KNOWN_ISSUES.md](../tracking/KNOWN_ISSUES.md) and the don't-regress rules in
[invariants/INVARIANTS.md](../invariants/INVARIANTS.md).

_Last updated: 2026-06-13._

---

## How to read this

Items are ordered by **blast radius first, then likelihood**. A memory-corruption abort that
fires on a routine config edit outranks a latent OOB in dead code. Each phase below is meant to
land as one or a few focused changes, each with its own regression test, so the next phase
starts from a known-good base.

The cross-cutting themes (lettered A–F in KNOWN_ISSUES) map onto the phases:
A = ownership, B = local-IPC trust boundary, C = hostile RX, D = blocking-in-loop,
E = non-blocking-fd vs blocking-write, F = advertised-but-dead code.

---

## Phase 1 — Stop the crashes: device-ownership (theme A)

**Fix first.** These corrupt the heap on ordinary, non-malicious use.

`c->serial.device` is sometimes the non-heap `argv[optind]` (`src/main.c:717`) and sometimes a
`strdup`'d string. Two code paths `free()` it unconditionally:

- **ZT-001** 🔴 — `profile_load` does `free((void *)c->serial.device)` before re-`strdup`
  (`src/ext/profile.c:93`). On an inotify hot-reload of a watched profile that carries a
  `device=` key, this frees `argv` → abort / heap corruption.
- **ZT-002** 🔴 — `port_rediscover` does the same `free((void *)c->serial.device)` on the first
  reconnect when `--port-glob`/`--match-vid-pid` resolves a *different* path
  (`src/serial/port_discover.c:171`).
- **ZT-016** ⚪ / **ZT-018** ⚪ — the mirror leaks: a startup `--profile` device strdup is
  leaked when a positional device is also given (`src/main.c:717`), and several early-return
  paths leak owned strings (`src/main.c:696`).

**Fix direction (single rule, applied once):** make `c->serial.device` *always* heap-owned —
`strdup(argv[optind])` at startup so every later `free`+reassign is valid. Then both ZT-001 and
ZT-002 become correct as written, ZT-016's double-source goes away, and ZT-018 collapses into a
single cleanup label. This establishes **[INVARIANTS §1](../invariants/INVARIANTS.md)**
(resource & pointer ownership) as a real, testable rule rather than an aspiration.

**Verify:** unit test that loads a profile with `device=/dev/ttyUSB9` over an argv-supplied
device and asserts no abort under ASan; a reconnect test with `--port-glob` that resolves a new
path; run the existing suite under `make test` with ASan (leaks become hard failures).

Detail: [ZT-001](../tracking/issues/ZT-001-profile-load-frees-argv-device.md),
[ZT-002](../tracking/issues/ZT-002-port-rediscover-frees-argv-device.md).

---

## Phase 2 — The trust boundary: unauthenticated local IPC (theme B)

zyterm's real security boundary is "anything that can reach a local socket or the loopback HTTP
port can drive the serial line." Today that boundary is wide open. See
**[INVARIANTS §7](../invariants/INVARIANTS.md)** (network bridge & local-IPC trust boundary);
this phase is the work that makes §7 hold. A `SECURITY.md` posture doc should land alongside
this phase stating the threat model: *the device is trusted, local peers are not, the network
bridge is opt-in and must authenticate.*

- **ZT-004** 🔴 — `POST /tx` and `POST /api/send` write a serial line with **no auth**
  (`src/net/http.c:907`). A CORS simple-request or a DNS-rebind from any web page the operator
  visits reaches the bridge and executes commands on the attached device.
  **Fix:** require a bearer token on state-changing routes; validate `Origin`/`Host`; never
  emit `Access-Control-Allow-Origin: *` on `/tx`, `/api/send`, or any mutating route.
- **ZT-013** 🟠 — the WebSocket upgrade does **no `Origin` check** (`src/net/http.c:861`), so any
  site can open `GET /ws` cross-origin and read the live RX stream.
  **Fix:** `Origin` allowlist + the same token before completing the upgrade handshake.
- **ZT-012** 🟠 — the detach socket `/tmp/zyterm.<name>.sock` is created under the default umask
  with no `SO_PEERCRED` (`src/net/session.c:48`) → any local user can inject bytes onto the
  wire. **Fix:** create under `$XDG_RUNTIME_DIR` in a `0700` dir, `0600` socket, and check peer
  credentials on connect.
- **ZT-028** ⚪ — the metrics UNIX socket is created without restrictive perms
  (`src/net/metrics.c:39`) → info leak under a loose umask. **Fix:** same `0700` dir / `0600`
  socket / peer-cred recipe as ZT-012.

**Sequencing note:** land token + Origin/Host validation (ZT-004, ZT-013) *before* any roadmap
feature that exposes a control plane (the REST/WS control plane in
[ROADMAP.md](./ROADMAP.md) explicitly depends on auth existing first).

**Verify:** an integration test that issues `POST /tx` without a token and asserts `401`; a
cross-origin `Origin:` on `/ws` is rejected; `ls -l` of both UNIX sockets shows `srw-------`;
a non-owner `connect()` is refused by the peer-cred check.

Detail: [ZT-004](../tracking/issues/ZT-004-unauth-http-tx-csrf.md).

---

## Phase 3 — Hostile device RX: terminal-injection default-deny (theme C)

- **ZT-003** 🔴 — device RX bytes are written to the operator's terminal **verbatim**; the RX
  pipeline only strips `\r` (`src/render/render.c:93`, `render_rx` body ~248–288 — see the
  byte loop that only special-cases `\r`/`\n`). A hostile or buggy device can emit OSC 52 to
  hijack the operator's clipboard, or a title-set / DECRQSS sequence to inject a forged command
  line. **Fix:** default-deny dangerous escapes — filter `ESC ]` (OSC, especially OSC 52),
  privacy-message and APC strings, and DCS by default; gate raw/passthrough output behind an
  explicit opt-in (the existing `Ctrl+A G` KGDB/raw passthrough is the right place to scope the
  exception). This realizes **[INVARIANTS §6](../invariants/INVARIANTS.md)** (terminal output &
  escape-sequence safety).

Note the relationship to **ZT-019** (theme F): `osc8_rewrite` (`src/proto/osc.c:256`) is the
*only* place zyterm itself would emit OSC 8, and it is dead (no call site) and OOB-buggy. The
default-deny filter and the ZT-019 cleanup should be designed together so the allow/deny policy
is in one place.

**Verify:** feed a recorded RX stream containing `ESC ] 52 ; c ; <b64> BEL` and assert the
clipboard is untouched and the bytes are rendered inert; feed a title-set sequence and assert
the operator's terminal title is unchanged; confirm `Ctrl+A G` still passes raw bytes when
explicitly enabled. Add these to the property/fuzz corpus (Phase 6).

Detail: [ZT-003](../tracking/issues/ZT-003-device-rx-escape-injection.md).

---

## Phase 4 — The UI-hang cluster: never block the event loop (theme D)

The single-threaded event loop must never block — see
**[INVARIANTS §3](../invariants/INVARIANTS.md)**. Three sites violate that today.

- **ZT-006** 🟠 — `filter_stop` calls blocking `waitpid(pid, &status, 0)` from a normal loop
  tick (`src/ext/filter.c:86`). If the `--filter` child ignores `SIGTERM`, the whole UI hangs.
  **Fix:** non-blocking reap — `WNOHANG`, escalate to `SIGKILL` after a short grace, and finish
  the reap on a later tick (the same `hooks_reap` swap-pop pattern in `src/ext/hooks.c:255`).
- **ZT-005** 🟠 — a failed `Ctrl+A A` autobaud leaves `serial.fd = -1` (`src/loop/input.c:130`);
  `poll()` then ignores the fd so reconnect never fires, yet the HUD still shows "connected."
  This is a hang in the *user's mental model* even when the loop spins. **Fix:** on autobaud
  failure, drive the reconnect/reopen path (or set the disconnected HUD state) instead of
  leaving a stranded fd. Note autobaud already pauses the reader thread correctly
  (`rx_thread_pause` at `src/loop/input.c:134`); the gap is the failure exit.
- **ZT-026** ⚪ — the TX path `poll()`s for `POLLOUT` with a 250 ms timeout and loops with **no
  overall deadline** (`src/loop/send.c:128`, trickle path ~96). Under stuck hardware flow
  control the UI hangs indefinitely. **Fix:** an overall TX deadline; on expiry, abandon the
  write, free the heap buffer, and flash "TX stalled."
- **ZT-024** ⚪ (same theme) — `metrics_tick` accepts a *blocking* client fd (missing
  `SOCK_NONBLOCK`, `src/net/metrics.c:102`) → a non-draining scraper can stall the loop.
  **Fix:** `accept4(..., SOCK_NONBLOCK)` and drop the client on `EAGAIN`.

**Verify:** a `--filter cat` child that traps `SIGTERM` is killed within the grace window and
the UI stays responsive; a failed autobaud transitions to "disconnected" and reconnect fires; a
write to a TTY held in `RTS`-off flow control flashes "TX stalled" within the deadline; a
metrics client that never reads does not block the loop.

Detail: [ZT-005](../tracking/issues/ZT-005-autobaud-strands-fd.md),
[ZT-006](../tracking/issues/ZT-006-filter-stop-blocking-waitpid.md).

---

## Phase 5 — HTTP/log correctness + leak cluster (themes E, F)

A family of bugs that share one root: a non-blocking fd handed to a write helper that only
understands `EINTR`, plus missing error handling on rotation. See
**[INVARIANTS §5](../invariants/INVARIANTS.md)** (decoder/IO bounds) and §7.

- **ZT-007** 🟠 — `http_broadcast` caps a chunk at 4096 B with **no loop**
  (`src/net/http.c:1020`, `chunk = n > 4096 ? 4096 : n`) → RX bursts >4 KiB are silently
  truncated in the web view. **Fix:** loop in ≤4096-byte segments over the full `n`.
- **ZT-009** 🟠 — WS broadcast calls `ws_frame_text` and **ignores its write result**
  (`src/net/http.c:1037`), unlike the SSE branch which checks and closes. Dead peers are never
  closed and a partial frame corrupts the stream. **Fix:** have `ws_frame_text` report failure;
  close `HC_WS` on error, mirroring SSE.
- **ZT-017** ⚪ — directly downstream of ZT-009: dead WebSocket connections are never reaped
  (`src/net/http.c:1036`); ~16 ungraceful disconnects exhaust all `HC_MAX` slots → bridge DoS.
  Fixing ZT-009 (close on write error) fixes this.
- **ZT-011** ⚪ — `zt_write_all` retries only `EINTR` (`src/core/core.c:187`, the function at
  `:192`) but is used to serve `--webroot` files over **non-blocking** HTTP fds → large files
  truncate on `EAGAIN`. **Fix:** an `EAGAIN`-aware write (poll for `POLLOUT` and continue), or
  serve webroot from a blocking one-shot fd. Audit every `zt_write_all` caller in `src/net/`
  for the same hazard.
- **ZT-010** 🟠 — log rotation ignores `rename()`/`open()` failures (`src/log/logio.c:40`); on
  `ENOSPC` the fd is closed, the reopen fails silently, and all logging stops with no warning.
  **Fix:** check both return values; on failure, keep the old fd if possible and emit a
  one-time warning.

**Verify:** broadcast a 16 KiB RX burst and assert the SSE/WS client receives all of it; kill a
WS client mid-frame and assert its slot is freed and the stream stays valid; serve a 5 MiB
webroot file over a slow client and assert byte-exact delivery; fill the log filesystem and
assert a warning instead of silent loss.

Detail: [ZT-007](../tracking/issues/ZT-007-http-broadcast-truncates-4k.md),
[ZT-009](../tracking/issues/ZT-009-ws-broadcast-ignores-errors.md).

---

## Phase 6 — Engineering hardening

Beyond the catalogued defects, these strengthen the parts that are *correct today but fragile*.

### SPSC backpressure + drop accounting

The `--threaded` reader ring drops bytes **silently** under backpressure. In `rx_thread_main`
(`src/loop/rx_thread.c:84`) the worker computes `free_sp = cap - (head - tail)` and writes
`wr = min(n, free_sp)` — when the main thread falls behind, the tail of each read is discarded
with no counter, no flash, and no log. **Fix:** account dropped bytes (an atomic counter the
main thread can read), surface them in the HUD/stats and as a Prometheus metric, and consider an
explicit overflow policy (drop-newest vs. drop-oldest) instead of an implicit truncation. This
keeps **[INVARIANTS §4](../invariants/INVARIANTS.md)** (reader thread & fd lifecycle / SPSC
ring) honest about what the ring guarantees.

### Reset stateful decoders on reconnect

Several decoders carry state across reads that becomes garbage after a disconnect: the framing
decoders (`feed_cobs`/`feed_slip`/`feed_hdlc`/`feed_len16` state on `c->proto`, e.g. the
half-read LENPFX header at `src/proto/framing.c:198`) and the telnet IAC filter
(`telnet_rx_filter(uint8_t *state, …)`, `src/serial/transport.c:163`). `framing_reset` exists
(`src/proto/framing.c:46`) but the reconnect path does not call it. **Fix:** call
`framing_reset` and reset the telnet IAC `state` on every successful reconnect, so a
mid-frame disconnect can't corrupt the first frame after reconnect. Pairs with the
**THREADING_AND_RECONNECT** design doc.

### Bound TX writes with a deadline

This is the engineering form of ZT-026: give both `trickle_send` and `direct_send`
(`src/loop/send.c:79`, `:109`) a single, shared "overall TX deadline" helper rather than an
unbounded `poll(…, 250)` retry loop. One implementation, used by both paths.

### Drain POLLHUP

**ZT-027** ⚪ — the non-threaded `POLLHUP` path reads **once** before reconnecting
(`src/loop/runtime.c:156`) and can lose buffered RX (e.g. a device's final log line on hangup).
The `POLLIN` path right below it (`src/loop/runtime.c:170`) already drains in a loop. **Fix:**
make the `POLLHUP` branch drain the same way before handing off to `run_reconnect_loop`.

### Property / fuzz tests for the byte-twiddling

The framing, telnet, EOL-mapping, and CRC code is exactly the kind of input-driven byte logic
that hides off-by-ones (ZT-021 COBS bound, ZT-022 zero-length LENPFX desync). **Add:**
- a round-trip property test per framing mode (`encode → feed → assert payload identical`);
- a telnet IAC fuzz target (`telnet_rx_filter`) checking no OOB and idempotent escaping;
- an EOL map-in/map-out round-trip across all six modes;
- a CRC known-answer test for ccitt/ibm/crc32 plus a fuzz target on the framed decoders.

These also become the regression corpus for the Phase 3 escape-filter.

### ThreadSanitizer CI job

CI is **ASan/UBSan-only** today (`.github/workflows/ci.yml` matrix: `gcc`/`clang` ×
`{none, asan-ubsan}`). The `--threaded` reader is the one place with real concurrency
(release/acquire atomics on the SPSC ring), and ASan does not catch data races. **Add:** a TSan
job that builds with `-fsanitize=thread` and runs the threaded pty/integration tests, so the
SPSC happens-before edges in `src/loop/rx_thread.c` are checked in CI.

### Release hardening flags + drop `-march=native`

The release build links with **no hardening flags** and `-march=native`
(`Makefile:85`, `release: CFLAGS += -flto -march=native -DNDEBUG`). `-march=native` is fatal for
a *distributed* binary — a `.deb` built on the CI runner will execute illegal instructions on an
older CPU. **Fix:**
- drop `-march=native` from the distributed release build (keep it only as a local-only opt-in);
- add `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=2` (with `-O2`+), and link with
  `-Wl,-z,relro,-z,now` + `-fPIE -pie`;
- keep these out of the sanitizer builds where they conflict.

This is the open half of **[INVARIANTS §9](../invariants/INVARIANTS.md)** (build & release) and
should be reflected in [ops/RELEASE.md](../ops/RELEASE.md) once landed.

---

## Out of scope here

Low-severity correctness items that are real but not on the hardening critical path are tracked
as board rows in [KNOWN_ISSUES.md](../tracking/KNOWN_ISSUES.md): ZT-014 (clipboard OOM
NULL-check), ZT-015 (filter EINTR byte drop), ZT-019 (dead `osc8_rewrite` OOB), ZT-020 (`--http`
`atoi` validation), ZT-021/022 (framing bounds — folded into the Phase 6 fuzz work), ZT-023
(fuzzy off-by-one, masked by the dead fuzzy finder), ZT-025 (TX-OOM silent drop). Dead/stubbed
features (fuzzy finder ZT-008, multi-pane, fastio, OSC 8) are not hardened — they are either
deleted or properly built per [ROADMAP.md](./ROADMAP.md).
