# Known Issues

The home for confirmed defects, seeded 2026-06-03 from a staff-level source-review audit;
recorded as found. All 28 audited defects (ZT-001 … ZT-028) have since been fixed on branch
`fix/zt-001-ownership-and-ui-hangs` — see **Resolved**. Each row is a real bug found by reading
`src/`, not a feature request — non-defect work lives in [STATUS.md](STATUS.md). High/critical
defects also get a detail file under [`issues/`](issues/); the rest are tracked as board rows.
Severity drives order, not discovery date.

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

_None open. All 28 audited defects are resolved below._

## Resolved

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
| ZT-019 | ⚪ | memsafety | **Fixed** — `osc8_rewrite`'s bounds check reserves `2*url_len` (the URL is emitted twice — target + visible text), closing the OOB write for `url_len > ~18`. Still has no call site. `src/proto/osc.c`. |
| ZT-020 | ⚪ | integer | **Fixed** — `--http` is parsed with `strtol` and range-checked to 1–65535 (`zt_die` on garbage/out-of-range) instead of an unchecked `atoi`. `src/main.c`. |
| ZT-021 | ⚪ | integer | **Fixed** — `encode_cobs` reserves the true worst case `n + n/254 + 2` instead of `n + 2`. `src/proto/framing.c`. |
| ZT-022 | ⚪ | logic | **Fixed** — a zero-length LENPFX frame dispatches immediately on header completion instead of consuming the next byte as payload and desyncing. `src/proto/framing.c`. |
| ZT-023 | ⚪ | memsafety | **Fixed** — the fuzzy-finder selection clamps with `>=`, leaving a free byte on a full-size entry. `src/tui/fuzzy.c`. |
| ZT-024 | ⚪ | error-handling | **Fixed** — `metrics_tick` accepts with `SOCK_NONBLOCK` so a non-draining scraper can't stall the loop (the write helper drops on EAGAIN). `src/net/metrics.c`. |
| ZT-025 | ⚪ | error-handling | **Fixed** — `tx_preprocess` flashes "TX dropped — out of memory" on an allocation failure instead of silently swallowing the send. `src/loop/send.c`. |
| ZT-026 | ⚪ | error-handling | **Fixed** — `trickle_send`/`direct_send` bound the EAGAIN retry with a progress-resetting stall deadline (`ZT_TX_STALL_DEADLINE_S`) and flash "TX stalled". `src/loop/send.c`. |
| ZT-027 | ⚪ | error-handling | **Fixed** — the non-threaded `POLLHUP` path drains the serial fd in a loop (like POLLIN) so buffered RX isn't lost before reconnect. `src/loop/runtime.c`. |
| ZT-028 | ⚪ | security | **Fixed** — the metrics socket is created 0600 via a scoped `umask` and `metrics_tick` rejects non-self peers via `SO_PEERCRED`. `src/net/metrics.c`. |

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
  the OSC 8 rewrite is bounds-correct (still uncalled) → [STATUS.md](STATUS.md).

_Last updated: 2026-06-13._
