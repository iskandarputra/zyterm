# Known Issues

The home for confirmed defects, seeded 2026-06-03 from a staff-level source-review audit;
recorded as found. All 28 audited defects (ZT-001 … ZT-028) have since been fixed on branch
`fix/zt-001-ownership-and-ui-hangs`; ZT-029 is a later finding (the mislabeled SGR passthrough),
fixed under [ADR-0009](../decisions/0009-device-rx-sgr-only-filter.md) — see **Resolved**. Each row is a real bug found by reading
`src/`, not a feature request — non-defect work lives in [STATUS.md](STATUS.md). High/critical
defects also get a detail file under [`issues/`](issues/); the rest are tracked as board rows.
Severity drives order, not discovery date.

**2026-07-03 re-review (v1.4.0):** a second multi-agent source review (30 raw findings, adversarially
verified → 23 kept → 20 after dedup) added **ZT-030 … ZT-049**, now open below. Its dominant theme is
that several of the ZT-001…029 class-fixes were applied to one code path and their sibling paths were
missed (e.g. the ZT-001 argv-free is still live on `--profile-save`; the ZT-027 drain-before-reconnect
is gated `if (!threaded)`; the ZT-007 truncation fix never reached the JSONL logger). Fix order and the
non-defect (arch/testability/perf) work from that pass live in
[plans/HARDENING_2026-07.md](../plans/HARDENING_2026-07.md); the full narrative is archived at
[archive/audit/2026-07-03-source-review.md](../archive/audit/2026-07-03-source-review.md).

**Legend:** 🔴 high / critical · 🟠 medium · ⚪ low · status `open` = recorded, not yet fixed.

<!--
HOW TO ADD A DEFECT
  1. Take the next monotonic ID: ZT-029, ZT-030, … (never reuse a retired number).
  2. Add a row to the "Open" table in severity order (🔴 then 🟠 then ⚪), using:
       | ZT-NNN | 🔴/🟠/⚪ | <area> | `src/…:line` | open | <what's wrong> → <fix direction>. |
  3. If it is high or critical (🔴), also write a detail file:
       docs/tracking/issues/ZT-NNN-<slug>.md   (template in ../../  decisions? no — see brief §3)
     and link the ID cell to it.
  4. When it is fixed, MOVE the row to the "Resolved" section with the fixing commit/PR — never
     delete it. IDs are permanent; the board is the historical record.
-->

## Open

| ID | Sev | Area | Location | Status | What's wrong → fix direction |
|----|-----|------|----------|--------|------------------------------|

_None open. ZT-030 … ZT-049 are fixed and merged to `main` (2026-07-04) — see **Resolved**._

## Resolved

### 2026-07 wave — v1.4.0 re-review (ZT-030 … ZT-049)

Fixed on branch `fix/zt-030-033-high-severity` (stacked: docs → high → medium → low). Verified with
clean `-Werror` release + debug builds and the full test suite (unit + e2e + pty), `clang-format`
clean; new regression tests cover the `--profile-save` crash, the fuzzy `sent_len` invariant, the
non-blocking filter fd, the split-body `POST /tx`, path-anchored routing, and C1 neutralization.

| ID | Sev | Area | Resolution |
|----|-----|------|------------|
| [ZT-030](issues/ZT-030-profile-save-frees-argv-device.md) | 🔴 | ownership | **Fixed** — `--profile-save` frees any prior heap device then `strdup`s `argv[optind]`, so `cleanup_ctx`'s `free` is valid. `src/main.c`. |
| [ZT-031](issues/ZT-031-fuzzy-inject-stale-sent-len-oob.md) | 🔴 | tui/memsafety | **Fixed** — the fuzzy-finder Enter injection resets `sent_len=0`, restoring the `sent_len+cursor ≤ input_len` invariant. `src/tui/fuzzy.c`. |
| [ZT-032](issues/ZT-032-filter-stdin-blocking-hangs-loop.md) | 🔴 | blocking-in-loop | **Fixed** — `in_pipe[1]` is set `O_NONBLOCK` after the fork, so `filter_feed` drops bytes instead of blocking the loop. `src/ext/filter.c`. |
| [ZT-033](issues/ZT-033-rx-thread-orphaned-on-siglongjmp-uaf.md) | 🔴 | concurrency/embedded | **Fixed** — `rx_thread_embed_reset()` stops the worker from `zt_die` and `zt_embed_reset`; `sig_crash` stays async-signal-safe and relies on that reset. `src/loop/rx_thread.c`, `src/core/core.c`. |
| ZT-034 | 🟠 | logic | **Fixed** — `POST /tx`/`/api/send` parse `Content-Length`, wait for the full body (413 if oversized) and cap the sent bytes. `src/net/http.c`. |
| ZT-035 | 🟠 | correctness | **Fixed** — `/ws` sends binary frames (`0x82`); `ws_frame_text` → `ws_frame_binary`. `src/net/http.c`. |
| ZT-036 | 🟠 | logic | **Fixed** — the threaded POLLHUP/POLLERR paths drain the SPSC ring into the log/render pipeline before reconnect. `src/loop/runtime.c`. |
| ZT-037 | 🟠 | blocking-in-loop | **Fixed** — non-blocking `connect()` bounded by `poll(POLLOUT)` to `ZT_CONNECT_TIMEOUT_MS`. `src/serial/transport.c`. |
| ZT-038 | 🟠 | logic | **Fixed** — autobaud requires a printable score ≥ `ZT_AUTOBAUD_MIN_SCORE`, else fails and keeps the default baud. `src/serial/autobaud.c`. |
| ZT-039 | ⚪ | logic | **Fixed** — routing parses the request-target (`parse_request_line`) and matches the exact path, gating POST on the method. `src/net/http.c`. |
| ZT-040 | ⚪ | correctness | **Fixed** — the JSONL payload is segmented into records that fit `esc`, each with an accurate `n`. `src/log/log_json.c`. |
| ZT-041 | ⚪ | security | **Fixed** — `emit_inert_byte` tracks UTF-8 state (`proto.utf8_cont`) and neutralizes standalone C1 `0x80–0x9F` as `M-` notation. `src/render/render.c`. |
| ZT-042 | ⚪ | memsafety | **Fixed** — `visible_len` clamps the multi-byte advance at the NUL. `src/tui/hud.c`. |
| ZT-043 | ⚪ | resource | **Fixed** — `http_tick` polls established SSE/WS fds for hangup/EOF and reaps them. `src/net/http.c`. |
| ZT-044 | ⚪ | blocking-in-loop | **Fixed** — `http_write_all` uses an absolute deadline captured once at entry. `src/net/http.c`. |
| ZT-045 | ⚪ | integer | **Fixed** — `kern_delta()` clamps a decreasing kernel counter to a zero delta across fd swaps. `src/serial/tty_stats.c`. |
| ZT-046 | ⚪ | error-handling | **Fixed** — the X11 worker resets `running=false` on exit so a later copy relaunches instead of reporting a dead selection as owned. `src/proto/clipboard.c`. |
| ZT-047 | ⚪ | fd-leak | **Fixed** — `setup_serial` `close(fd)`s before each `zt_die` (errno preserved). `src/serial/serial.c`. |
| ZT-048 | ⚪ | leak | **Fixed** — the `--replay` branch frees a prior `--profile` heap device before aliasing `replay_path`. `src/main.c`. |
| ZT-049 | ⚪ | fd-leak | **Fixed** — the clipboard wake-pipe is closed on every worker-exit path; `wake_worker` is mutex-guarded. `src/proto/clipboard.c`. |

### 2026-07-04 — fuzz-driven find (ZT-050)

Surfaced by the new `xmodem_receive` libFuzzer target (`tests/fuzz/fuzz_xmodem.c`) on its first runs;
fixed and regression-guarded in the same change (`tests/integration/test_xmodem.c`, with a `SIGALRM`
watchdog), ASan-clean.

| ID | Sev | Area | Resolution |
|----|-----|------|------------|
| [ZT-050](issues/ZT-050-xmodem-receive-bad-complement-hang.md) | 🟠 | hang/DoS | **Fixed** — `xmodem_receive` no longer `continue`s on a bad block-number complement (which re-tested the same unchanged block forever); the complement check is merged into the CRC-mismatch NAK path, so a corrupt complement byte is NAK'd and the sender's retransmission is read. `src/proto/xmodem.c`. |


Fixed on branch `fix/zt-001-ownership-and-ui-hangs` (stacked on the docs rebuild). Verified with a
clean `-Werror` build + the full test suite (unit + integration + pty) under AddressSanitizer/UBSan
— zero sanitizer reports — plus targeted regression tests for the escape filter, HTTP auth, segmented
broadcast and the fuzzy finder.

| ID | Sev | Area | Resolution |
|----|-----|------|------------|
| [ZT-001](issues/ZT-001-profile-load-frees-argv-device.md) | 🔴 | ownership | **Fixed** — `c->serial.device` is now always heap-owned: `src/main.c` `strdup`s `argv[optind]` (freeing any prior `--profile` value first) and frees it once in teardown, so `profile_load()`'s `free()`+`strdup()` operates on heap memory. |
| [ZT-002](issues/ZT-002-port-rediscover-frees-argv-device.md) | 🔴 | ownership | **Fixed** by the same single-ownership change — `port_rediscover()` (`src/serial/port_discover.c`) now frees a heap pointer on reconnect/replug. |
| [ZT-003](issues/ZT-003-device-rx-escape-injection.md) | 🔴 | security | **Fixed** — `render_rx()` (`src/render/render.c`) default-denies device escapes: ESC and other C0/DEL controls are rendered as inert `cat -v` caret notation (`^[`, `^G`, …) before reaching the terminal, neutralizing OSC 52 clipboard hijack, title injection and cursor/screen spoofs. `\t`/UTF-8 pass; the `passthrough`/`sgr_passthrough` opt-ins are the only unfiltered modes (INVARIANTS §6). |
| [ZT-004](issues/ZT-004-unauth-http-tx-csrf.md) | 🔴 | security | **Fixed** — `POST /tx` / `/api/send` now pin `Host`/`Origin` to a loopback literal (CSRF + DNS-rebind defence) and, when `--http-token` is set, require `Authorization: Bearer <token>` (401 otherwise); foreign origins get 403. `cors_block` no longer advertises `POST` to `*`. `src/net/http.c` (INVARIANTS §7). |
| [ZT-005](issues/ZT-005-autobaud-strands-fd.md) | 🟠 | logic | **Fixed** — a failed `Ctrl+A A` autobaud now recovers like `Ctrl+A r` instead of leaving `serial.fd == -1`. `src/loop/input.c`. |
| [ZT-006](issues/ZT-006-filter-stop-blocking-waitpid.md) | 🟠 | logic | **Fixed** — `filter_stop()` reaps with a bounded `WNOHANG` grace window then escalates to `SIGKILL`; no blocking `waitpid(…,0)` on a loop tick. `src/ext/filter.c`. |
| [ZT-007](issues/ZT-007-http-broadcast-truncates-4k.md) | 🟠 | logic | **Fixed** — `http_broadcast` / `http_broadcast_tx` iterate the whole payload in ≤4096-byte segments for both SSE and WS instead of encoding only the first chunk. `src/net/http.c`. |
| ZT-008 | 🟠 | logic | **Fixed** — the fuzzy finder scans history from index 1 (index 0 is always NULL) and `handle_stdin_chunk()` now routes keystrokes to `fuzzy_handle()`, so the overlay filters, selects and cancels. `src/tui/fuzzy.c`, `src/loop/input.c`. |
| [ZT-009](issues/ZT-009-ws-broadcast-ignores-errors.md) | 🟠 | error-handling | **Fixed** — `ws_frame_text()` returns an error and `http_broadcast` closes the WS peer on a failed/partial frame (shared fix with ZT-017). `src/net/http.c`. |
| ZT-010 | 🟠 | error-handling | **Fixed** — `log_rotate_if_needed()` checks `rename()`/`open()` and warns via `log_notice` instead of silently losing the log after the first rotation. `src/log/logio.c`. |
| ZT-011 | 🟠 | error-handling | **Fixed** — one-shot HTTP responses use a new bounded, EAGAIN-aware `http_write_all()` (waits for `POLLOUT` with a 2 s deadline) so a large `--webroot` file isn't truncated on the non-blocking fd. `src/net/http.c`. |
| ZT-012 | 🟠 | security | **Fixed** — the detach socket now lives under `$XDG_RUNTIME_DIR` (0700), is created 0600 via a scoped `umask`, and `session_tick()` rejects attachers whose uid isn't ours via `SO_PEERCRED`. `src/net/session.c`. |
| ZT-013 | 🟠 | security | **Fixed** — the WS upgrade and the SSE `/stream` validate `Origin`/`Host` (shared helper with ZT-004) before streaming device output. `src/net/http.c`. |
| ZT-014 | ⚪ | concurrency | **Fixed** — the X11 worker splits the alloc from the copy and NULL-checks it (`snap_len` stays 0 on OOM), so a failed `malloc` no longer crashes the process. `src/proto/clipboard.c`. |
| ZT-015 | ⚪ | error-handling | **Fixed** — `filter_feed` retries on `EINTR` (only `EAGAIN` drops), so a signal no longer truncates a chunk to the filter subprocess. `src/ext/filter.c`. |
| ZT-016 | ⚪ | leak | **Fixed** by the ZT-001 single-ownership change — the prior `--profile` device string is freed before overwrite and released once in teardown. |
| ZT-017 | ⚪ | fd-leak | **Fixed** with ZT-009 — dead WebSocket peers are closed on the first failed frame, so ungraceful disconnects no longer exhaust the 16 slots. `src/net/http.c`. |
| ZT-018 | ⚪ | leak | **Fixed** — a single `cleanup_ctx()` helper frees every parse-owned heap field; the `--replay`/`--attach`/`--diff`/`-h`/`-V`/`--profile-save` early returns call it (replay nulls the non-heap `device` alias first). `src/main.c`. |
| ZT-019 | ⚪ | memsafety / dead-code | **Fixed** — first the OOB write was closed (guard reserved `2*url_len`); then, 2026-07-04, the whole feature was **removed**: `osc8_rewrite` had no call site and the "OSC 8" settings toggle read nothing (a no-op that misreported its state), so the routine, the toggle, the `proto.hyperlinks` flag, and the osc8 tests were deleted. `src/proto/osc.c`, `src/tui/hud.c`, `src/loop/input.c`. |
| ZT-020 | ⚪ | integer | **Fixed** — `--http` is parsed with `strtol` and range-checked to 1–65535 (`zt_die` on garbage/out-of-range) instead of an unchecked `atoi`. `src/main.c`. |
| ZT-021 | ⚪ | integer | **Fixed** — `encode_cobs` reserves the true worst case `n + n/254 + 2` instead of `n + 2`. `src/proto/framing.c`. |
| ZT-022 | ⚪ | logic | **Fixed** — a zero-length LENPFX frame dispatches immediately on header completion instead of consuming the next byte as payload and desyncing. `src/proto/framing.c`. |
| ZT-023 | ⚪ | memsafety | **Fixed** — the fuzzy-finder selection clamps with `>=`, leaving a free byte on a full-size entry. `src/tui/fuzzy.c`. |
| ZT-024 | ⚪ | error-handling | **Fixed** — `metrics_tick` accepts with `SOCK_NONBLOCK` so a non-draining scraper can't stall the loop (the write helper drops on EAGAIN). `src/net/metrics.c`. |
| ZT-025 | ⚪ | error-handling | **Fixed** — `tx_preprocess` flashes "TX dropped — out of memory" on an allocation failure instead of silently swallowing the send. `src/loop/send.c`. |
| ZT-026 | ⚪ | error-handling | **Fixed** — `trickle_send`/`direct_send` bound the EAGAIN retry with a progress-resetting stall deadline (`ZT_TX_STALL_DEADLINE_S`) and flash "TX stalled". `src/loop/send.c`. |
| ZT-027 | ⚪ | error-handling | **Fixed** — the non-threaded `POLLHUP` path drains the serial fd in a loop (like POLLIN) so buffered RX isn't lost before reconnect. `src/loop/runtime.c`. |
| ZT-028 | ⚪ | security | **Fixed** — the metrics socket is created 0600 via a scoped `umask` and `metrics_tick` rejects non-self peers via `SO_PEERCRED`. `src/net/metrics.c`. |
| ZT-029 | 🟠 | security | **Fixed** — "SGR Passthrough" forwarded *all* device escapes (the `raw_ok` gate disabled neutralization wholesale), not just SGR, and `sgr_filter()` was a dead no-op stub — so enabling it reopened the ZT-003 surface (OSC 52 / title / cursor). Replaced with a bounded, pure SGR-only filter (`sgr_feed`): only `CSI…m` with `0-9;:` params passes, private/intermediate markers (`CSI?1m`) and overflow abort to inert. Now safe to default-on; `--no-sgr` restores deny-all → [ADR-0009](../decisions/0009-device-rx-sgr-only-filter.md). `src/proto/sgr_passthrough.c`, `src/render/render.c`. |

_None of these were user-visible API changes except the new `--http-token` flag and the now-required
loopback `Origin`/`Host` on the bridge's write/stream routes; see [`../../CHANGELOG.md`](../../CHANGELOG.md)
`[Unreleased]`._

---

## Cross-cutting themes

The 28 defects clustered into six recurring shapes; all are now closed, and each drives a
don't-regress rule in [INVARIANTS.md](../invariants/INVARIANTS.md):

- **A — Mixed pointer ownership** (ZT-001, ZT-002, ZT-016, ZT-018): startup strings are now
  single-owned, freed once via `cleanup_ctx()` / teardown → [INVARIANTS §1](../invariants/INVARIANTS.md).
- **B — Unauthenticated local IPC is the trust boundary** (ZT-004, ZT-012, ZT-013, ZT-028): the HTTP
  bridge is origin-pinned + optionally token-gated; the detach and metrics sockets are 0600 +
  peer-cred checked → [INVARIANTS §7](../invariants/INVARIANTS.md).
- **C — Hostile device RX echoed verbatim** (ZT-003): device escapes are default-denied on the render
  path → [INVARIANTS §6](../invariants/INVARIANTS.md).
- **D — Blocking calls in the single-threaded loop** (ZT-006, ZT-024, ZT-026): bounded/`WNOHANG`/
  `SOCK_NONBLOCK` everywhere a loop tick could otherwise stall → [INVARIANTS §3](../invariants/INVARIANTS.md).
- **E — Non-blocking fd + blocking write helper** (ZT-009, ZT-011, ZT-017): the bridge has
  EAGAIN-aware one-shot writes and closes dead peers → [INVARIANTS §5](../invariants/INVARIANTS.md), §7.
- **F — Advertised-but-dead code** (ZT-008, ZT-019, ZT-023): the fuzzy finder is wired and bounded;
  the OSC 8 rewrite + its inert toggle were removed (2026-07-04) → [STATUS.md](STATUS.md).

_Last updated: 2026-07-04 — ZT-030 … ZT-049 recorded, fixed, and merged to `main`; the 2026-06 Resolved set is unchanged._
