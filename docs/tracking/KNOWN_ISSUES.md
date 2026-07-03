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
| [ZT-030](issues/ZT-030-profile-save-frees-argv-device.md) | 🔴 | ownership | `src/main.c:723` | open | `--profile-save <name> <DEVICE>` aliases non-heap `argv[optind]` into `c.serial.device`; `cleanup_ctx` then `free()`s it (`main.c:329`) → `free(): invalid pointer` / heap corruption. ZT-001 twin, still live on this one path → `strdup` it, or `NULL` before cleanup like the replay path (`main.c:747`). |
| [ZT-031](issues/ZT-031-fuzzy-inject-stale-sent-len-oob.md) | 🔴 | tui/memsafety | `src/tui/fuzzy.c:65` | open | fuzzy-finder Enter injects a history line setting `input_len`/`cursor` but not `sent_len`; with local echo or after Tab (`sent_len>0`) the next Backspace/Ctrl+W computes unsigned `input_len-(sent_len+cursor)` ≈ `SIZE_MAX` → OOB `memmove` over `input_buf` → crash/`zt_ctx` corruption → set `sent_len=0` like `input.c:40`. |
| [ZT-032](issues/ZT-032-filter-stdin-blocking-hangs-loop.md) | 🔴 | blocking-in-loop | `src/ext/filter.c:70` | open | `--filter` stdin (`in_pipe[1]`) is left blocking (no `O_NONBLOCK`), so `filter_feed`'s EAGAIN drop-branch (`:127`) is dead and a slow filter blocks `write()` in the poll loop → whole UI (incl. Ctrl+A x) hangs → `O_NONBLOCK` on `in_pipe[1]` after fork (child stdin is a separate OFD). INVARIANTS §3. |
| [ZT-033](issues/ZT-033-rx-thread-orphaned-on-siglongjmp-uaf.md) | 🔴 | concurrency/embedded | `src/core/core.c:312` | open | embedded fatal `siglongjmp` (`zt_die`/`sig_crash`) and `zt_embed_reset` skip `rx_thread_stop`, orphaning the `--threaded` worker still writing into `zyterm_main`'s reclaimed stack → UAF in the host + dup-fd/1 MiB-ring leak → run one teardown (stop worker, close fds) on every exit path. INVARIANTS §4. |
| ZT-034 | 🟠 | logic | `src/net/http.c:1071` | open | `POST /tx`/`/api/send` take the body only from bytes buffered when `\r\n\r\n` first appears and ignore `Content-Length`; a body split across TCP segments is truncated/dropped while returning 204 (a partial command can reach the wire) → parse `Content-Length`, accumulate the full body (cap + 413) before `direct_send`. |
| ZT-035 | 🟠 | correctness | `src/net/http.c:1161` | open | `/ws` ships raw 8-bit serial in a TEXT frame (`hdr[0]=0x81`); any non-UTF-8 byte makes conformant browsers fail the stream (close 1007), so `/ws` is unusable for typical serial → send binary frames (`0x82`) or base64 like the SSE path. |
| ZT-036 | 🟠 | logic | `src/loop/runtime.c:189` | open | the threaded POLLHUP/POLLERR path frees the SPSC ring without draining it (the ZT-027 drain is gated `if (!threaded)`), so the device's final RX burst is lost from log/scrollback → `rx_thread_drain` into `log_write`/`rx_ingest` before `rx_thread_stop` in both branches. |
| ZT-037 | 🟠 | blocking-in-loop | `src/serial/transport.c:117` | open | `connect()` is synchronous (the socket is made non-blocking only afterward), so a firewalled `tcp://`/`telnet://` peer freezes the reconnect loop for the kernel connect timeout (~127 s) — quit/resize dead → non-blocking connect + `poll(POLLOUT)` with a bounded deadline. INVARIANTS §3. |
| ZT-038 | 🟠 | logic | `src/serial/autobaud.c:52` | open | a silent/binary device scores `0.0` at every rate; the `>`/tie-break accepts monotonically higher rates, so autobaud always locks to the top rate (4 Mbaud) and returns success — suppressing the "no printable traffic" warning → require `score>0` to accept, else fail and keep the default baud. |
| ZT-039 | ⚪ | logic | `src/net/http.c:984` | open | route dispatch runs unanchored `strstr` over the whole request buffer before the POST branch → `GET /streamlit.html` served as SSE, and a `POST /tx` whose body contains `"GET /stream"` is hijacked into an SSE upgrade (command never written) → parse the request-target and match with a path boundary; gate POST on method. |
| ZT-040 | ⚪ | correctness | `src/log/log_json.c:70` | open | the JSONL logger escapes only the first 2048 payload bytes but writes the true full `n`, so bursts >2048 B are recorded with `b` shorter than the advertised `n` — silent, self-inconsistent loss (ZT-007 twin) → segment like `http_broadcast`, or emit a truncation marker and a matching count. |
| ZT-041 | ⚪ | security | `src/render/render.c:247` | open | `emit_inert_byte` neutralizes only ESC/DEL/C0; C1 8-bit introducers `0x80–0x9F` pass verbatim, so on a C1-honoring terminal a raw `0x9D … 0x07` is an 8-bit OSC 52 clipboard write that bypasses the SGR filter → neutralize `0x80–0x9F` while still passing valid UTF-8 continuation bytes. _(plausible; needs a C1-honoring terminal.)_ |
| ZT-042 | ⚪ | memsafety | `src/tui/hud.c:84` | open | `visible_len()` advances 2/3/4 bytes on a UTF-8 lead without checking the continuation bytes exist before NUL; a history entry truncated mid-glyph in `fuzzy_draw`'s 256-byte `line2` → OOB stack read → clamp the multi-byte advance at the terminator. |
| ZT-043 | ⚪ | resource | `src/net/http.c:1147` | open | established SSE/WS fds are never polled for hangup; dead/half-open peers are reaped only on a failing broadcast write, so on an idle link 16 half-open clients exhaust `HC_MAX` and the bridge stops serving → poll `HC_SSE`/`HC_WS` for `POLLHUP` each tick and/or emit a keepalive ping. |
| ZT-044 | ⚪ | blocking-in-loop | `src/net/http.c:195` | open | `http_write_all` resets its stall deadline on every partial write, so `HTTP_WRITE_DEADLINE_MS` bounds only a consecutive no-progress stall; a trickle-reading local client keeps the poll loop inside a ~16.7 KB response for tens of seconds → capture an absolute deadline at entry, or use a per-connection non-blocking write queue. _(plausible.)_ |
| ZT-045 | ⚪ | integer | `src/serial/tty_stats.c:72` | open | kernel error-counter deltas use unsigned arithmetic against baselines never reset on fd swap; after reconnect the fresh device counters restart near 0, so `0 - prev` underflows to ~4.3e9 and the HUD flashes a fake fault → reset `kern_*` baselines on every (re)open, or clamp a decrease to 0. |
| ZT-046 | ⚪ | error-handling | `src/proto/clipboard.c:460` | open | the native X11 worker's in-loop error breaks leave `g.running=true`/`g.init_failed` unset, so later `clipboard_native_set` never relaunches and reports "native X11 owner" success while nothing owns the selection → on any worker exit set `g.running=false`/reset so callers relaunch or fall through to OSC 52 / helper / file. |
| ZT-047 | ⚪ | fd-leak | `src/serial/serial.c:160` | open | `setup_serial` calls `zt_die` on its post-open error paths without `close(fd)`; harmless under standalone `exit`, but under embedded `siglongjmp` the fd leaks per invocation → EMFILE in a long-lived host → `close(fd)` before each `zt_die`. |
| ZT-048 | ⚪ | leak | `src/main.c:736` | open | the `--replay` branch overwrites `c.serial.device` with the replay path without freeing a prior `--profile`-supplied `strdup`'d device (the ZT-016 free-before-assign was missed here) → `free` before assign, like `main.c:762/769`. |
| ZT-049 | ⚪ | fd-leak | `src/proto/clipboard.c:494` | open | the clipboard wake-pipe (`g.wakefd[0]/[1]`) is opened before the X worker starts; on worker init failure (`init_failed` latched) it is never closed and later calls early-return, leaking the two fds for the process lifetime → close/`-1` the pipe on any worker init-failure path. _(plausible.)_ |

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
  the OSC 8 rewrite is bounds-correct (still uncalled) → [STATUS.md](STATUS.md).

_Last updated: 2026-07-03 — added ZT-030 … ZT-049 from the v1.4.0 re-review (Resolved set unchanged since 2026-06-13)._
