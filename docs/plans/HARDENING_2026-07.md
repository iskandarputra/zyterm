# Reliability hardening plan — wave 2 (2026-07, v1.4.0)

> **Status (2026-07-03): defects fixed, engineering work open.** All twenty defects (ZT-030 … ZT-049)
> in Phases 1–4 below are fixed on branch `fix/zt-030-033-high-severity` (pending merge) — see the
> Resolved table in [tracking/KNOWN_ISSUES.md](../tracking/KNOWN_ISSUES.md). The **non-defect**
> engineering work is still open: the `http.c` split (Phase 2), SPSC drop accounting + a TSan CI leg
> (Phase 3), the performance hot-path work (Phase 5), the testability foundation (Phase 6), and the
> architecture-accuracy / dead-code cleanup (Phase 7). This is the sequel to
> [RELIABILITY_HARDENING.md](./RELIABILITY_HARDENING.md) (wave 1, ZT-001…028, COMPLETE), not a
> replacement — that file stays as the historical record of the first wave.

Every defect item references an ID (`ZT-030`…`ZT-049`) in
[tracking/KNOWN_ISSUES.md](../tracking/KNOWN_ISSUES.md) and the don't-regress rules in
[invariants/INVARIANTS.md](../invariants/INVARIANTS.md). The full source-review narrative behind this
plan is archived at
[archive/audit/2026-07-03-source-review.md](../archive/audit/2026-07-03-source-review.md); features
(as opposed to fixes) are in [ROADMAP.md](./ROADMAP.md).

_Last updated: 2026-07-03._

---

## The theme: class-fixes applied to one path, siblings missed

Wave 1 closed 29 defects. This pass found **no new whole-subsystem rot** — instead it found that
several wave-1 fixes were applied point-wise to one code path and their twins were left unfixed. That
observation is the highest-leverage process change: **fix bugs as a class, then grep for every sibling
site.** The concrete twins:

- **ZT-001** (argv/heap `free`) → still live on `--profile-save` (**ZT-030**).
- **ZT-027** (drain-before-reconnect) → gated `if (!threaded)`, so the threaded path still loses the
  final RX burst (**ZT-036**).
- **ZT-007** (`http_broadcast` 4 KiB truncation) → never applied to the JSONL logger (**ZT-040**).
- **ZT-006/015** (filter reap / EINTR) → `filter_feed`'s stdin fd was left blocking, making its own
  drop-branch dead code (**ZT-032**).
- **ZT-003** (device-RX escape default-deny) → neutralizes ESC/C0 but not the C1 `0x80–0x9F`
  introducers (**ZT-041**).

Ordering below is **blast radius first, then likelihood**, same rule as wave 1.

---

## Phase 1 — Stop the crash / UAF / hang (the four 🔴)

These crash, corrupt memory, or wedge the UI on ordinary use. Three are one-line/one-guard fixes.

- **ZT-030** 🔴 — `zyterm --profile-save myprof /dev/ttyUSB0` aliases the non-heap `argv[optind]`
  into `c.serial.device` (`src/main.c:723`), then `cleanup_ctx` `free()`s it (`src/main.c:329`) →
  `free(): invalid pointer`. The replay branch already guards the identical alias (`src/main.c:747`).
  **Fix:** `strdup(argv[optind])`, or `NULL` it before cleanup. Then grep every
  `c.serial.device = argv[...]` site and assert the free/NULL discipline.
  Detail: [ZT-030](../tracking/issues/ZT-030-profile-save-frees-argv-device.md).
- **ZT-031** 🔴 — `fuzzy_handle`'s Enter branch injects a history line without resetting `sent_len`
  (`src/tui/fuzzy.c:65`); its twin `load_history_into_buf` sets `sent_len=0` for exactly this reason
  (`src/loop/input.c:40`). With local echo on or after Tab (`sent_len>0`), the next Backspace/Ctrl+W
  runs `memmove` with an unsigned `input_len-(sent_len+cursor)` ≈ `SIZE_MAX` over `input_buf`.
  **Fix:** `c->tui.sent_len = 0` in the Enter branch; add an editing-invariant assertion.
  Detail: [ZT-031](../tracking/issues/ZT-031-fuzzy-inject-stale-sent-len-oob.md).
- **ZT-032** 🔴 — `--filter`'s stdin write end is left blocking (`src/ext/filter.c:70`), so
  `filter_feed`'s EAGAIN drop-branch (`:127`) is dead and a slow filter blocks `write()` inside the
  poll loop → the whole UI, including Ctrl+A x, hangs. **Fix:** `O_NONBLOCK` on `in_pipe[1]` after the
  fork (the child's stdin is a separate OFD and stays blocking). INVARIANTS §3.
  Detail: [ZT-032](../tracking/issues/ZT-032-filter-stdin-blocking-hangs-loop.md).
- **ZT-033** 🔴 — both embedded fatal `siglongjmp` exits (`zt_die` `src/core/core.c:226`, `sig_crash`
  `:312`) and `zt_embed_reset` (`:93`) skip `rx_thread_stop`, so the `--threaded` worker keeps writing
  into `zyterm_main`'s reclaimed stack (`src/main.c:352`/`:881`) → UAF in the long-lived host + a
  leaked dup fd and 1 MiB ring. **Fix:** run one teardown (stop worker, close fds, free ring) on
  *every* exit path; in `sig_crash` keep only async-signal-safe work and do the full teardown on the
  host side after the jump. This is the anchor for the **unified embed teardown** in Phase 4.
  Detail: [ZT-033](../tracking/issues/ZT-033-rx-thread-orphaned-on-siglongjmp-uaf.md).

**Verify:** `--profile-save` with a positional device runs clean under ASan; injecting a history line
with local echo on then Backspace does not underflow; a `--filter` child that never reads stdin does
not freeze the UI (Ctrl+A x still quits); an embedded SIGABRT with `--threaded` leaves no live worker
(TSan/ASan clean across a re-entrant `zyterm_main`).

---

## Phase 2 — HTTP request-path correctness (the operator-facing bridge)

`net/http.c` (1262 LOC) carries the trust boundary *and* the densest cluster of residual bugs. Fix the
behaviour, then split the file so the boundary is small and testable.

- **ZT-034** 🟠 — `POST /tx`/`/api/send` read the body only from whatever was buffered when
  `\r\n\r\n` first appeared and ignore `Content-Length` (`src/net/http.c:1071`) → a body split across
  TCP segments is truncated/dropped while returning 204 (a partial command can reach the wire).
  **Fix:** parse `Content-Length`, keep the slot open until headers+body are buffered (cap + 413 on
  overflow), then `direct_send`.
- **ZT-035** 🟠 — `/ws` sends raw 8-bit RX in a TEXT frame (`src/net/http.c:1161`); browsers close
  1007 on the first non-UTF-8 byte. **Fix:** binary frames (`0x82`) or base64 like SSE.
- **ZT-039** ⚪ — unanchored `strstr` route dispatch over the whole buffer, before the POST branch
  (`src/net/http.c:984`) → `GET /streamlit.html` served as SSE and a `POST /tx` body containing
  `"GET /stream"` hijacked into an upgrade. **Fix:** parse the request-target once, match with a path
  boundary, gate POST on the method.
- **ZT-043** ⚪ — established SSE/WS fds are never polled for hangup; dead peers are reaped only on a
  failing broadcast write (`src/net/http.c:1147`) → 16 half-open clients wedge the bridge on an idle
  link. **Fix:** poll `HC_SSE`/`HC_WS` for `POLLHUP` each tick and/or emit a keepalive ping.
- **ZT-044** ⚪ — `http_write_all` resets its stall deadline on every partial write
  (`src/net/http.c:195`), so a trickle-reading client keeps the loop inside a ~16.7 KB response for
  tens of seconds. **Fix:** capture an absolute deadline at entry, or a per-connection non-blocking
  write queue drained across ticks. INVARIANTS §3.

**Decomposition (arch):** split `net/http.c` along its seams — `http_server`/parse, an
`http_routes` handler table, `http_ws`, `http_sse`, and an `http_auth` unit owning
`request_origin_ok`/`request_token_ok`/`cors_block` (the INVARIANTS §7 code). The Makefile
auto-discovers files, so no build change is needed. Do the ZT-034/035/039/043/044 fixes as the first
slices of that split.

**Verify:** headers+body in separate writes deliver the full body to `direct_send`; a non-UTF-8 byte
survives on `/ws`; `GET /streamlit.html` is not treated as a stream; 16 killed SSE clients free their
slots; a slow reader cannot exceed the total deadline. (These are the parser tests in Phase 6.)

---

## Phase 3 — Threaded / SPSC data-loss and the missing concurrency verification

The `--threaded` SPSC path is a second-class citizen: a correctness gap plus zero concurrency testing.

- **ZT-036** 🟠 — the threaded POLLHUP/POLLERR path frees the ring without draining
  (`src/loop/runtime.c:189`; the ZT-027 drain is gated `if (!threaded)`), so the device's final RX
  burst is lost. **Fix:** loop `rx_thread_drain` into `log_write`/`rx_ingest` before `rx_thread_stop`
  in both branches (or have `rx_thread_stop` flush the ring into the pipeline instead of discarding).
- **SPSC drop accounting** — the producer silently drops the tail under backpressure
  (`src/loop/rx_thread.c:84–88`) with no counter/flash/metric. **Fix:** an atomic
  `rx_dropped_bytes_total`, surfaced in the HUD and Prometheus. *(This was already listed in
  [RELIABILITY_HARDENING.md](./RELIABILITY_HARDENING.md) Phase 6 "SPSC backpressure + drop
  accounting" and is re-confirmed still open.)*
- **Concurrency tests + TSan** — no test moves a byte through the ring (only lifecycle is exercised,
  `tests/unit/test_subsystems.c`), and CI has no ThreadSanitizer. **Fix:** a producer/consumer ring
  test (write 256 KiB through a pipe, drain in a loop, assert byte-exact ordering incl. a wrap and an
  overflow-drop case) and a `-fsanitize=thread` CI leg that runs it. *(Also a re-confirmation of
  wave-1 Phase 6 "ThreadSanitizer CI job", which did not land.)*

INVARIANTS §4 (reader thread & fd lifecycle) should be updated to state the drop policy honestly once
accounting exists.

---

## Phase 4 — Sweep the remaining class-fix siblings + the unified embed teardown

Each is a known-pattern residual with a clear fix.

- **ZT-038** 🟠 — autobaud accepts a zero score and always locks to the top rate on a silent/binary
  device (`src/serial/autobaud.c:52`), suppressing its own "no printable traffic" warning. **Fix:**
  require `score>0` to accept, else fail and keep the default baud.
- **ZT-040** ⚪ — the JSONL logger truncates the payload to 2048 B while writing the true `n`
  (`src/log/log_json.c:70`; the ZT-007 twin). **Fix:** segment like `http_broadcast`, or emit a
  truncation marker with a matching count.
- **ZT-041** ⚪ — C1 `0x80–0x9F` introducers bypass the device-RX escape filter
  (`src/render/render.c:247`), reopening OSC-52 clipboard injection on C1-honoring terminals.
  **Fix:** neutralize `0x80–0x9F` in `emit_inert_byte` while still passing valid UTF-8 continuation
  bytes (key the decision on filter mode / UTF-8 state). INVARIANTS §6.
- **ZT-045** ⚪ — kernel error-counter deltas underflow on reconnect (`src/serial/tty_stats.c:72`) →
  a fake ~4.3e9 fault flash. **Fix:** reset `kern_*` baselines on every (re)open, or clamp a decrease
  to 0.
- **Embedded leaks (ZT-046, ZT-047, ZT-048, ZT-049)** — the X11 worker leaves `running=true` after a
  connection error (`src/proto/clipboard.c:460`); `setup_serial` leaks its fd on error under
  `siglongjmp` (`src/serial/serial.c:160`); the replay branch leaks a `--profile` device string
  (`src/main.c:736`); the clipboard wake-pipe leaks on worker init failure
  (`src/proto/clipboard.c:494`). **Fix:** all four fold into the **single embed teardown** anchored by
  ZT-033 — one function that stops the worker and closes/frees every per-run resource, invoked on
  normal return, on both `siglongjmp` exits, and from `zt_embed_reset`.

---

## Phase 5 — Performance on the RX hot path

Correct today but O(n)-per-byte where it should be O(lines)/O(chunks).

- **`log_write` one `write(2)` per RX byte** (`src/log/logio.c:73`) — ~300k syscalls/s at 3 Mbaud with
  a log open, enough to saturate a core and cause kernel RX overruns. **Fix:** emit at most one
  `write`/`writev` per line-span, or a userspace buffer flushed once per chunk.
- **`tty_stats_poll` runs 2 ioctls every loop iteration** (`src/serial/tty_stats.c:66`) instead of at
  HUD cadence — the `t_last_stats` field it stores is never read to rate-limit. **Fix:** gate the body
  on `t_last_stats` at HUD cadence.
- **SPSC producer copies byte-by-byte** (`src/loop/rx_thread.c:86`) — mirror the consumer's two
  `memcpy` spans; optionally coalesce wake-pipe writes.
- **`http_broadcast` base64-encodes with zero clients connected** (`src/net/http.c` broadcast path,
  guarded only by "server listening") — maintain an `HC_SSE`+`HC_WS` count and early-return before
  `b64enc` when it is 0.
- **Scrollback malloc/free per line** (`src/log/scrollback.c:170`) — a flat byte ring + `(offset,len)`
  descriptors makes eviction pointer arithmetic and bounds memory to one region.

---

## Phase 6 — The testability foundation (the highest-leverage structural investment)

The hostile-input parsers and the lock-free ring have essentially no unit/fuzz/TSan coverage, so every
fix above ships unguarded against regression. The root blocker is that `framing_feed` hard-calls
`render_rx`, forcing slow socket/`usleep` integration tests.

- **The sink seam** — add a test-injectable output sink (a function pointer on `c->proto`, or an
  overridable `render_rx`) that captures emitted bytes instead of writing the TTY. Route
  framing/filter/xmodem output through it. This unblocks fast, deterministic unit tests and is the
  prerequisite for everything else here. Also resolves the `proto→render` back-edge (see Phase 7).
- **Framing round-trip + regression tests** (`tests/unit/test_framing.c`) — each mode
  encode→feed→assert byte-identical, plus explicit ZT-021 (COBS run-marker) and ZT-022 (zero-length
  LENPFX) cases, split-frame decode, and known-bad frames asserting `crc_err`/counter behaviour.
- **XMODEM/YMODEM/ZMODEM tests** (`tests/integration/test_xmodem.c`) — a scripted socketpair peer for
  send and receive, covering block sequencing, complement-byte, CRC, and the retry/timeout/CAN paths.
- **SPSC ring test + TSan leg** — see Phase 3.
- **Fuzzing** (`tests/fuzz/`) — libFuzzer targets for `framing_feed`, `classify_request`/`hc_pump_new`,
  and `xmodem_receive` under `-fsanitize=fuzzer,address,undefined`, with a small seed corpus and a
  short CI leg.
- **HTTP parser tests** — split-body, partial-header, oversized-header (431), and no-terminator
  (slowloris drain) cases, driving `classify_request`/`hc_pump_new` over a synthetic `hc_t` (fast,
  no sockets). These will exercise the ZT-034/039/043/044 fixes directly.
- **Coverage gate** — a `make coverage` target and a CI step printing per-file line coverage with a
  ratchet baseline, so whole-file blind spots (http.c, input.c, hud.c) become visible.

---

## Phase 7 — Architecture accuracy & dead code

Two marquee guarantees are documented but currently false; make them real (cheaply) or correct the
docs. Both come with a fix, so they are tracked here rather than as defects.

- **The "compiler-enforced" layering is not enforced.** Every `.c` includes the umbrella
  `src/zt_internal.h` (which transitively pulls `loop.h` and thus every layer), and three back-edges
  already exist via bare `extern`: `proto/framing.c` → `render_rx`, `proto/macros.c` → the `send`
  primitives, `ext/hooks.c` → `direct_send`. **Fix:** have each `.c` include only its own narrow
  module header (umbrella stays for `main.c`), add a CI lint flagging any up-layer `extern` or
  non-`main` umbrella include, and resolve the back-edges by relocating the shared cores downward (the
  RX sink from Phase 6 removes `proto→render`; move the encode+write send core into `proto`/`serial`
  so `macros.c`/`hooks.c` call a same-or-lower-layer helper). Update **INVARIANTS §8** /
  **ARCHITECTURE §2/§9** to match reality once the lint lands — until then §8 overstates enforcement.
- **"All per-process state lives in one `zt_ctx`" is contradicted by ~20 file-statics**, of which
  `zt_embed_reset` scrubs only two — a real cross-run bug (`proto/passthrough.c:40` KGDB `~.` state
  bleeds between embedded runs). **Fix:** pull genuinely per-session state into `zt_ctx`; for state
  that must stay module-private (http `g_conn`, clipboard handle), replace the hard-coded resets with
  a small registry each module registers into, plus a lint mirroring the layering check.
- **Dead code shipping in every binary** — `serial/fastio.c` (epoll+splice, 0 callers),
  `proto/osc.c` `osc8_rewrite` (0 callers), and `ext/multi.c` (0 callers, and it reintroduces the
  file-static session state + a blocking loop read the invariants forbid). **Fix:** delete or
  `#ifdef`-gate all three (the `fastio` epoll/splice decision is already open in
  [ROADMAP.md](./ROADMAP.md) and [ADR-0003](../decisions/0003-epoll-splice-fastpath-deferred.md));
  the one real `fastio` win — `splice`ing serial→logfile on the raw-dump path — is the only piece
  worth wiring. Also close the `profile_save`/`profile_load` round-trip gaps (`flow` is documented but
  neither written nor parsed; watches/macros aren't persisted).

---

## Out of scope here

Net-new **features** proposed by the same review live in [ROADMAP.md](./ROADMAP.md): interactive
replay controls, a `--plain`/accessible mode + consistent `NO_COLOR`, expanded observability
(Prometheus counters + an NDJSON lifecycle-event stream), and an auto-loaded base config with
device-keyed profiles. The already-listed roadmap items the review re-affirmed — DTR/RTS + auto-reset
recipes, the capture-group expect engine + `--extract`, `--send-file` pacing, and `rfc2217://` — are
not duplicated here.
