# Known Issues

The home for confirmed defects, seeded 2026-06-03 from a staff-level source-review audit;
recorded, not yet fixed. Each row is a real bug found by reading `src/`, not a feature request —
non-defect work lives in [STATUS.md](STATUS.md). High/critical defects also get a detail file under
[`issues/`](issues/); the rest are tracked as board rows. Severity drives order, not discovery date.

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
| [ZT-001](issues/ZT-001-profile-load-frees-argv-device.md) | 🔴 | ownership | `src/ext/profile.c:93` | open | `free((void*)c->serial.device)` on inotify profile reload, but device is often the non-heap `argv[optind]` (`src/main.c:717`) → heap corruption / abort when a watched profile with a `device=` key is edited → make device single-owned (always strdup). |
| [ZT-002](issues/ZT-002-port-rediscover-frees-argv-device.md) | 🔴 | ownership | `src/serial/port_discover.c:171` | open | Same free-of-argv on first reconnect when `--port-glob` + a positional device resolve a different path → same single-ownership rule. |
| [ZT-003](issues/ZT-003-device-rx-escape-injection.md) | 🔴 | security | `src/render/render.c:93` | open | Device RX bytes written to the operator's terminal verbatim (only `\r` stripped), no ESC/OSC filtering → OSC 52 clipboard hijack, title injection → default-deny dangerous escapes; gate raw passthrough. |
| [ZT-004](issues/ZT-004-unauth-http-tx-csrf.md) | 🔴 | security | `src/net/http.c:907` | open | `POST /tx` / `POST /api/send` writes the serial line with no auth; reachable cross-site via CORS simple-request / DNS-rebind → command exec on the device → bearer token + Origin/Host validation; no `*` CORS on state-changing routes. |
| [ZT-005](issues/ZT-005-autobaud-strands-fd.md) | 🟠 | logic | `src/loop/input.c:130` | open | A failed `Ctrl+A A` autobaud leaves `serial.fd=-1`; `poll()` ignores it so reconnect never fires while the HUD still shows connected → drive reconnect / reopen on the failure path. |
| [ZT-006](issues/ZT-006-filter-stop-blocking-waitpid.md) | 🟠 | logic | `src/ext/filter.c:86` | open | `filter_stop()` does a blocking `waitpid(…,0)` from a normal loop tick → UI hang if the filter ignores SIGTERM → WNOHANG + SIGKILL escalation / non-blocking reap. |
| [ZT-007](issues/ZT-007-http-broadcast-truncates-4k.md) | 🟠 | logic | `src/net/http.c:1020` | open | `http_broadcast` caps at 4096 B with no loop; RX bursts >4 KiB are silently truncated in the web view → loop in ≤4096-byte segments. |
| ZT-008 | 🟠 | logic | `src/tui/fuzzy.c:79` | open | Fuzzy finder scans history from index 0 (always NULL) and is not routed in `input.c` → the feature is entirely non-functional → iterate from index 1 + wire routing. |
| [ZT-009](issues/ZT-009-ws-broadcast-ignores-errors.md) | 🟠 | error-handling | `src/net/http.c:1037` | open | WS broadcast ignores `ws_frame_text` write errors (unlike SSE) → dead peers are never closed and partial frames corrupt the stream → check the return and close on error. |
| ZT-010 | 🟠 | error-handling | `src/log/logio.c:40` | open | Log rotation ignores `rename()` / `open()` failures → silent log loss after the first rotation (e.g. ENOSPC) → check both and warn. |
| ZT-011 | 🟠 | error-handling | `src/core/core.c:192` | open | `zt_write_all` retries only EINTR but is used on non-blocking HTTP fds (`src/net/http.c`) → large `--webroot` files truncated on EAGAIN → EAGAIN-aware write, or use blocking one-shot fds. |
| ZT-012 | 🟠 | security | `src/net/session.c:48` | open | Detach socket `/tmp/zyterm.<name>.sock` created under the default umask with no SO_PEERCRED → local byte injection to the serial line → `$XDG_RUNTIME_DIR` 0700 dir + 0600 socket + peer-cred check. |
| ZT-013 | 🟠 | security | `src/net/http.c:861` | open | WS upgrade does no Origin check → any site can read the live RX stream cross-origin → Origin allowlist + token. |
| ZT-014 | ⚪ | concurrency | `src/proto/clipboard.c:333` | open | `memcpy(malloc(g.len), …)` with an unchecked NULL → crash on OOM in the X11 worker → split the call and NULL-check the allocation. |
| ZT-015 | ⚪ | error-handling | `src/ext/filter.c:106` | open | `filter_feed` drops bytes on EINTR (should retry) → separate EINTR (continue) from EAGAIN (break). |
| ZT-016 | ⚪ | leak | `src/main.c:717` | open | A startup `--profile` device strdup is leaked when a positional device is also given → single ownership + free before overwrite. |
| ZT-017 | ⚪ | fd-leak | `src/net/http.c:1036` | open | Dead WebSocket connections are never reaped; 16 ungraceful disconnects exhaust all slots → bridge DoS → close `HC_WS` on write error (mirror the SSE path). |
| ZT-018 | ⚪ | leak | `src/main.c:696` | open | `--replay` / `--attach` / `--diff` / `-h` / `-V` early returns leak `filter_cmd` / `metrics_path` / `session_name` / `http_webroot` / hooks; matters in embedded mode → consolidate on one cleanup label. |
| ZT-019 | ⚪ | memsafety | `src/proto/osc.c:256` | open | `osc8_rewrite` writes each URL twice past its bounds check (OOB when `url_len`>~18). Dead code (no call site) so latent today → emit the URL once / fix the guard. |
| ZT-020 | ⚪ | integer | `src/main.c:586` | open | `--http` parsed via `atoi()`, truncated to uint16, no validation → `strtol` + range check 1–65535. |
| ZT-021 | ⚪ | integer | `src/proto/framing.c:241` | open | `encode_cobs` cap check undercounts the worst case (`n + n/254 + 2`); not currently reachable (caller oversizes) → correct the bound. |
| ZT-022 | ⚪ | logic | `src/proto/framing.c:199` | open | A zero-length LENPFX frame swallows the next byte → desync → dispatch immediately when `len16_need == 0`. |
| ZT-023 | ⚪ | memsafety | `src/tui/fuzzy.c:61` | open | `>` vs `>=` off-by-one leaves no NUL room on a 4096-byte entry (masked today by ZT-008) → use `>=`. |
| ZT-024 | ⚪ | error-handling | `src/net/metrics.c:102` | open | `metrics_tick` accepts a blocking client fd (missing `SOCK_NONBLOCK`) → potential stall on a non-draining client → add `SOCK_NONBLOCK` + drop on EAGAIN. |
| ZT-025 | ⚪ | error-handling | `src/loop/send.c:83` | open | `tx_preprocess` OOM silently drops TX with no diagnostic → distinguish OOM from empty; flash on drop. |
| ZT-026 | ⚪ | error-handling | `src/loop/send.c:128` | open | TX `poll()` timeout loops with no overall deadline → UI hang under stuck flow control → bounded deadline + "TX stalled" flash. |
| ZT-027 | ⚪ | error-handling | `src/loop/runtime.c:156` | open | The non-threaded `POLLHUP` path reads once and can lose buffered RX before reconnect → drain in a loop like the `POLLIN` path. |
| ZT-028 | ⚪ | security | `src/net/metrics.c:39` | open | The metrics UNIX socket is created without restrictive perms → info leak under a loose umask → 0700 dir / 0600 socket + peer-cred. |

## Resolved

_None yet. When a defect is fixed, move its row here (with the fixing commit/PR) — never delete it._

---

## Cross-cutting themes

The 28 defects cluster into six recurring shapes. They drive the don't-regress rules in
[INVARIANTS.md](../invariants/INVARIANTS.md) and the fix order in
[RELIABILITY_HARDENING.md](../plans/RELIABILITY_HARDENING.md):

- **A — Mixed pointer ownership** (ZT-001, ZT-002, ZT-016, ZT-018): `c->serial.device` and a few
  startup strings are sometimes heap, sometimes `argv` → see [INVARIANTS §1](../invariants/INVARIANTS.md).
- **B — Unauthenticated local IPC is the trust boundary** (ZT-004, ZT-012, ZT-013, ZT-028): HTTP
  bridge, detach socket, and metrics socket all reach the serial line without auth → see
  [INVARIANTS §7](../invariants/INVARIANTS.md).
- **C — Hostile device RX echoed verbatim** (ZT-003): unfiltered escapes from the device reach the
  operator's terminal → see [INVARIANTS §6](../invariants/INVARIANTS.md).
- **D — Blocking calls in the single-threaded loop** (ZT-006, ZT-024, ZT-026): a blocking syscall on
  a loop tick stalls the whole UI → see [INVARIANTS §3](../invariants/INVARIANTS.md).
- **E — Non-blocking fd + blocking write helper** (ZT-009, ZT-011, ZT-017): `zt_write_all` and WS
  broadcast assume blocking semantics on non-blocking fds → see [INVARIANTS §5](../invariants/INVARIANTS.md).
- **F — Advertised-but-dead code** (OSC 8, fastio, fuzzy, multi-pane): unwired paths carry latent
  bugs (ZT-008, ZT-019, ZT-023) and must not be sold as features → see [STATUS.md](STATUS.md).

_Last updated: 2026-06-03._
