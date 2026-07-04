# Reliability hardening plan — wave 2 (2026-07, v1.4.0)

> **Status (2026-07-03): defects fixed, engineering work open.** All twenty defects (ZT-030 … ZT-049)
> in Phases 1–4 below are fixed on branch `fix/zt-030-033-high-severity` (pending merge) — see the
> Resolved table in [tracking/KNOWN_ISSUES.md](../tracking/KNOWN_ISSUES.md). The **non-defect**
> engineering work is still open: the `http.c` split (Phase 2), SPSC drop accounting + a TSan CI leg
> (Phase 3), the performance hot-path work (Phase 5), and the testability foundation (Phase 6).
> **Phase 7 (architecture accuracy) is largely done** — the module layering is now compiler-enforced
> (narrow includes + `-Werror=implicit-function-declaration` + `make layering-check`), the embed-reset
> registry is in, and `fastio.c`/`multi.c` are deleted. This is the sequel to
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

The `--threaded` SPSC path was a second-class citizen: a correctness gap plus zero concurrency
testing. **DONE (2026-07):**

- **ZT-036** 🟠 — **fixed** (in the defect wave, PR #13): the threaded POLLHUP/POLLERR path now drains
  the ring into `log_write`/`rx_ingest` before `rx_thread_stop`, so the device's final RX burst isn't
  lost.
- **SPSC drop accounting — DONE:** the producer now records the tail it drops when the ring is full in
  an atomic `c->serial.spsc_dropped`, surfaced as the Prometheus `zyterm_rx_dropped_bytes_total`
  counter and a rate-limited HUD flash (`tty_stats_poll`), so backpressure loss is visible, not
  silent. The overflow case in the ring test asserts it's non-zero.
- **Concurrency tests + TSan — DONE** (PR #15): `tests/unit/test_rx_ring.c` moves bytes through the
  ring with a real worker (byte-exact + wrap + overflow-drop) and a `-fsanitize=thread` CI job runs
  it.

INVARIANTS §4 already states the drop policy; it's now backed by the counter + test.

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

Correct today but O(n)-per-byte where it should be O(lines)/O(chunks). **Mostly DONE (2026-07):**

- **`log_write` one `write(2)` per RX byte — DONE** (`src/log/logio.c`): both the RX and TX log paths
  now emit one write per line-span (timestamp, then everything to the next `\n`) via `memchr`, turning
  the ~300k-syscall/s storm at 3 Mbaud into O(lines). Byte-for-byte identical output, guarded by
  `tests/unit/test_logio.c`.
- **`tty_stats_poll` 2 ioctls every loop iteration — DONE** (`src/serial/tty_stats.c`): gated on
  `t_last_stats` at `ZT_HUD_REFRESH_MS` cadence (the field existed for this but was never read), so it
  fires ~2 Hz instead of thousands/s under high-baud RX.
- **SPSC producer byte-by-byte copy — DONE** (`src/loop/rx_thread.c`): mirrors the consumer's two
  contiguous `memcpy` spans across the wrap instead of a per-byte masked store.
- **`http_broadcast` base64-encodes with zero clients — DONE** (`src/net/http.c`): a
  `http_has_stream_peer()` guard early-returns from both `http_broadcast`/`http_broadcast_tx` before
  any `b64enc` when no SSE/WS peer is connected.
- **Scrollback malloc/free per line — still open** (`src/log/scrollback.c`): a flat byte ring +
  `(offset,len)` descriptors would remove per-line allocator churn, but it touches every `sb_lines`
  reader across `tui/scrollback_view.c` (draw + selection) and search — worth its own reviewed PR.

---

## Phase 6 — The testability foundation (the highest-leverage structural investment)

The hostile-input parsers and the lock-free ring had essentially no unit/fuzz/TSan coverage, so every
fix above shipped unguarded against regression. First tranche landed 2026-07:

- **The sink seam — DONE.** The Phase-7 dependency-inversion sinks (`c->core.rx_sink`/`tx_direct`)
  double as test seams: a test points them at capture buffers and exercises encode/decode with no
  sockets and no render path. (No separate `c->proto` hook was needed.)
- **Framing round-trip + regression tests — DONE** (`tests/unit/test_framing.c`, 21 assertions): each
  mode encode→feed→assert byte-identical over a delimiter-heavy payload, explicit **ZT-021** (COBS
  >254 run marker) and **ZT-022** (zero-length LENPFX no-desync) cases, CRC append/strip + mismatch
  detection, and a split-across-feeds case.
- **SPSC ring test + TSan leg — DONE** (`tests/unit/test_rx_ring.c`, 9 assertions): a real worker
  drains a pipe into the ring while main drains the ring — byte-exact round-trip, a forced wrap past
  `ZT_SPSC_CAP`, and drop-on-overflow (prefix intact, no corruption). A new **`tsan` CI job** builds
  the embed archive + this test under `-fsanitize=thread` and runs it, checking the release/acquire
  happens-before edges ASan can't see. The layering check also runs in CI now.
- **XMODEM tests — DONE** (`tests/integration/test_xmodem.c`, 10 asserts): a forked socketpair peer
  drives the engine — `xmodem_send` validated block-by-block (framing/complement/CRC/EOT + payload
  reassembly), a NAK→retransmit recovery, a receiver-CAN abort (returns -1, no hang), and
  `xmodem_receive` accepting a 3-block transfer with the 0x1A padding trimmed. (YMODEM/ZMODEM batch
  headers are a follow-on.)
- **Still open:** libFuzzer targets (`tests/fuzz/` for `framing_feed`, `classify_request`/`hc_pump_new`,
  `xmodem_receive`); HTTP parser unit tests (oversized-header 431, slowloris drain over a synthetic
  `hc_t` — split-body/routing already covered by the socket tests); and a `make coverage` ratchet gate.

---

## Phase 7 — Architecture accuracy & dead code

Two marquee guarantees were documented but false; this phase made them true. **Largely DONE
(2026-07)** — the layering is now compiler-enforced and the embed-reset registry is in; only a couple
of minor per-session file-static pulls and the `osc8_rewrite` removal remain.

- **The "compiler-enforced" layering is now actually enforced. DONE (2026-07).** A strict build
  surfaced **15 up-calls across 8 files** — far more than the review's "3 back-edges." All are
  resolved and the narrow includes + lint are on:

  | Up-call | Resolution |
  |---|---|
  | `render_rx`, `direct_send`/`trickle_send` back-edges | inverted through `c->core` sinks (`rx_sink`/`tx_direct`/`tx_trickle`). |
  | core → `session_`/`rx_thread_embed_reset` | embed-reset registry (below). |
  | `set_flash` ×6 (serial/proto/log → render) | moved down to `core` (pure ctx mutation, like `zt_warn`). |
  | `log_notice` (serial → log) | moved down to `core` (uses only the core output buffer). |
  | `rx_thread_pause`/`unpause` ×2 (autobaud/serial → loop) | `autobaud.c` relocated to the loop layer. |
  | `filter_feed`, `http_broadcast` (`rx_ingest` → ext/net) | `rx_ingest` moved to `loop/runtime.c` (file-static). |
  | `hooks_on_line` (`flush_line` → ext) | inverted via the `c->core.line_hook` sink. |
  | `http_notify_input` (`hud.c`/tui → net) | inverted via the `c->core.input_notify` sink. |
  | `emit_colored_line`, `osc52_copy` (`scrollback.c`/log → render/proto) | `scrollback.c` split — storage stays in `log`, the draw/selection/copy viewport moved to `tui/scrollback_view.c`. |

  With zero up-calls, every non-`main` `.c` now includes only its own narrow module header;
  `-Werror=implicit-function-declaration` (Makefile `WARN`) makes any future up-call a compile error,
  and `make layering-check` (in `make check`) rejects umbrella includes / bare up-layer `extern`s.
  **INVARIANTS §8** and **ARCHITECTURE §2** are updated to match — the claim is now true.
- **"All per-process state lives in one `zt_ctx`" — registry landed. DONE (2026-07)** for the reset
  mechanism: the hard-coded two-entry list is a **registry** — modules with per-run file-statics
  (`net/session.c`, `loop/rx_thread.c`) self-register a reset hook via a constructor, and
  `zt_embed_reset()`/`zt_die()` run the table, so core no longer names them. **Still open (minor):**
  pull the last genuinely-per-session file-statics into `zt_ctx` — `proto/passthrough.c`'s KGDB `~.`
  parser `state` (a real cross-run bug: it bleeds between embedded runs) and `log/record_cast.c`'s
  cast `t0`; add a lint so a new file-static must either live in `zt_ctx` or register a reset hook.
- **Dead code shipping in every binary** — **`serial/fastio.c` and `ext/multi.c` deleted (2026-07)**:
  both had zero callers, and `multi.c` reintroduced the file-static session state + a blocking loop
  read the invariants forbid; the `epoll` runtime was never worth it over `--threaded`, and the one
  real idea (`splice` serial→logfile on the raw-dump path) is noted in [ROADMAP.md](./ROADMAP.md).
  **Still open:** `proto/osc.c` `osc8_rewrite` (0 callers, ZT-019) — its removal is entangled with
  the user-facing "Hyperlinks (OSC 8)" settings toggle (`hud.c` row `E`, `input.c`), so it needs a
  small settings-menu renumber to remove cleanly. Also still open: the `profile_save`/`profile_load`
  round-trip gaps (`flow` documented but neither written nor parsed; watches/macros not persisted).

---

## Out of scope here

Net-new **features** proposed by the same review live in [ROADMAP.md](./ROADMAP.md): interactive
replay controls, a `--plain`/accessible mode + consistent `NO_COLOR`, expanded observability
(Prometheus counters + an NDJSON lifecycle-event stream), and an auto-loaded base config with
device-keyed profiles. The already-listed roadmap items the review re-affirmed — DTR/RTS + auto-reset
recipes, the capture-group expect engine + `--extract`, `--send-file` pacing, and `rfc2217://` — are
not duplicated here.
