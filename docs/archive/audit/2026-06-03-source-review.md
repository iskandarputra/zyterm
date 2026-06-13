# Source-review audit — 2026-06-03

> **History only.** This is the long-form narrative of the staff-level source review that seeded
> [`../../tracking/KNOWN_ISSUES.md`](../../tracking/KNOWN_ISSUES.md) (defects `ZT-001…ZT-028`). The
> durable don't-regress rules it produced were lifted into
> [`../../invariants/INVARIANTS.md`](../../invariants/INVARIANTS.md); the fix order and forward work
> live in [`../../plans/RELIABILITY_HARDENING.md`](../../plans/RELIABILITY_HARDENING.md) and
> [`../../plans/ROADMAP.md`](../../plans/ROADMAP.md). Statuses here reflect the codebase **as audited
> on 2026-06-03** (v1.2.0) — consult the tracker for current status.

## Method

The review combined first-hand reading of the highest-risk subsystems (the shared `zt_ctx`, the
SPSC reader thread, signals/embed recovery, the HTTP/SSE/WS bridge, the send path, hooks, the filter
subprocess, framing, transport, clipboard, scrollback, render) with a fan-out across seven finder
dimensions — concurrency/races, memory/fd leaks, memory safety, integer/arithmetic, logic/protocol
correctness, error handling, and security — where **every candidate finding was adversarially
re-verified against the actual source** before being recorded. 31 candidates were checked; 29
confirmed; deduplication (the clipboard NULL-deref was found twice) yields **28 unique defects**.

## Systemic themes

The defects are not random; they cluster into six root causes. The fixes should target the themes,
not just the instances.

| Theme | Instances |
| --- | --- |
| **A. Mixed pointer ownership** — a `const char *` that is sometimes argv, sometimes heap, but `free()`d as if always heap | ZT-001, ZT-002, ZT-016, ZT-018 |
| **B. Unauthenticated local IPC is the trust boundary** — loopback/`/tmp` sockets treated as trusted | ZT-004, ZT-012, ZT-013, ZT-028 |
| **C. Hostile device RX echoed to the operator's terminal verbatim** | ZT-003 |
| **D. Blocking calls inside the single-threaded event loop** → UI hang | ZT-006, ZT-024, ZT-026 |
| **E. Non-blocking fd + a blocking write helper** → truncation / dead-peer leak | ZT-009, ZT-011, ZT-017 |
| **F. Advertised-but-dead code** | OSC 8 (ZT-019), `fastio.c` epoll/splice, fuzzy finder (ZT-008), multi-pane |

## Findings by severity

Severities reflect real-world impact (crash, corruption, hang, security), not theoretical purity.
Locations are `src/…:line` as of the audit. Full per-issue detail for the critical/high items and
the key mediums lives under [`../../tracking/issues/`](../../tracking/issues/).

### Critical

- **ZT-001 — `profile_load()` frees an argv-owned device pointer** (`ext/profile.c:93`). `c->serial.device`
  is assigned directly from `argv[optind]` (`main.c:717`) on the common path, but the inotify config
  watcher re-runs `profile_load()` on every edit, and a profile with a `device=` key executes
  `free((void *)c->serial.device)` on non-heap memory → glibc abort / heap corruption. Editing a
  watched profile (any `:w`) triggers it. Fix: make `device` single-owned (always `strdup`).

### High

- **ZT-002 — `port_rediscover()` frees the argv-owned device pointer** (`serial/port_discover.c:171`).
  Same ownership bug on the first reconnect when `--port-glob` + a positional device resolve a
  different path (`ttyUSB0`→`ttyUSB1`) — the documented hot-plug workflow.
- **ZT-003 — device RX escape-sequence injection** (`render/render.c:93`, `render_rx` ~248-288). The
  default render path appends device bytes to the line buffer stripping only `\r`, then `ob_write`s
  them raw to the terminal — no ESC/OSC filtering. A hostile device or a MITM on `tcp://`/`telnet://`
  can drive OSC 52 (clipboard hijack), title-set, and DEC sequences. Fix: default-deny dangerous
  escapes; gate raw passthrough.
- **ZT-004 — unauthenticated `POST /tx` → serial-line CSRF** (`net/http.c:907`). The body is written
  straight to the serial fd with no auth. A `text/plain` POST is a CORS simple request (no preflight),
  so any visited web page — or DNS rebinding — can inject commands onto the attached device while
  `--http` is running. Fix: bearer token + `Origin`/`Host` validation; no `*` CORS on writes.

### Medium

- **ZT-005 — failed `Ctrl+A A` autobaud strands the fd** (`loop/input.c:130`). The handler closes
  `serial.fd`→`-1` then probes; on probe failure the fd stays `-1`, `poll()` ignores it, and the
  reconnect machinery (driven by POLLHUP/ERR on a valid fd) never engages. RX silently dead; HUD still
  shows connected. Fix: drive reconnect/reopen on failure.
- **ZT-006 — `filter_stop()` blocking `waitpid(…,0)` from the loop** (`ext/filter.c:86`). Reachable
  from a normal tick (filter EOF/write-error); a filter that ignores SIGTERM hangs the whole UI. Fix:
  `WNOHANG` + SIGKILL escalation / non-blocking reap.
- **ZT-007 — HTTP/SSE/WS truncates RX bursts >4096 B** (`net/http.c:1020`). `http_broadcast` caps at
  4 KiB with no loop; reads are up to 64 KiB, so the web view silently loses the tail of large bursts.
  Fix: loop in ≤4096-byte segments.
- **ZT-008 — fuzzy finder is non-functional** (`tui/fuzzy.c:79`). The scan loop starts at history index
  0 (always NULL, history is 1-based) and routing is incomplete, so `Ctrl+A .` never matches. Fix:
  iterate from 1 + wire the routing.
- **ZT-009 — WS broadcast ignores write errors** (`net/http.c:1037`). Unlike the SSE branch, the WS
  branch discards `ws_frame_text`'s result → dead peers are never closed (slot leak) and partial
  writes corrupt the frame stream. Fix: check the return and close, mirroring SSE.
- **ZT-010 — log rotation ignores `rename()`/`open()` failures** (`log/logio.c:40`). On reopen failure
  (e.g. ENOSPC — exactly when rotation matters) the fd stays `-1` and all subsequent logging is
  silently dropped. Fix: check both; warn.
- **ZT-011 — `zt_write_all` (blocking) on non-blocking HTTP fds** (`core/core.c:192`). It retries only
  EINTR; on EAGAIN it returns `-1`, so a `--webroot` file larger than the socket buffer is silently
  truncated. Fix: EAGAIN-aware write or blocking one-shot fds.
- **ZT-012 — detach socket permissions** (`net/session.c:48`). `/tmp/zyterm.<name>.sock`, default
  umask, no `SO_PEERCRED` → another local user can inject bytes onto the serial line. Fix:
  `$XDG_RUNTIME_DIR` 0700 + socket 0600 + peer-cred.
- **ZT-013 — WS upgrade has no Origin check** (`net/http.c:861`). Browsers don't enforce same-origin on
  WebSockets, so any visited site can read the live RX stream cross-origin. Fix: Origin allowlist + token.

### Low / latent

- **ZT-014** clipboard worker `memcpy(malloc(g.len), …)` unchecked NULL → crash on OOM (`proto/clipboard.c:333`).
- **ZT-015** `filter_feed` drops bytes on EINTR instead of retrying (`ext/filter.c:106`).
- **ZT-016** startup `--profile` device strdup leaked when a positional device is also given (`main.c:717`).
- **ZT-017** dead WebSocket connections never reaped → 16 ungraceful disconnects exhaust all slots (`net/http.c:1036`).
- **ZT-018** `--replay`/`--attach`/`--diff`/`-h`/`-V` early returns leak `filter_cmd`/`metrics_path`/`session_name`/`http_webroot`/hooks; matters in embedded mode (`main.c:696`).
- **ZT-019** `osc8_rewrite` writes each URL twice past its bounds check → latent OOB; **dead code** (no call site), so currently unreachable (`proto/osc.c:256`).
- **ZT-020** `--http` parsed with `atoi()`, truncated to `uint16` (`main.c:586`).
- **ZT-021** `encode_cobs` capacity check undercounts the COBS worst case; not currently reachable (`proto/framing.c:241`).
- **ZT-022** a zero-length LENPFX frame swallows the following byte → desync (`proto/framing.c:199`).
- **ZT-023** fuzzy finder `>` vs `>=` off-by-one leaves no NUL room (masked today by ZT-008) (`tui/fuzzy.c:61`).
- **ZT-024** `metrics_tick` accepts a blocking client fd → potential stall (`net/metrics.c:102`).
- **ZT-025** `tx_preprocess` OOM silently drops TX with no diagnostic (`loop/send.c:83`).
- **ZT-026** TX `poll()` timeout loops with no overall deadline → UI hang under stuck flow control (`loop/send.c:128`).
- **ZT-027** non-threaded POLLHUP path reads once; can lose buffered RX before reconnect (`loop/runtime.c:156`).
- **ZT-028** metrics UNIX socket created without restrictive perms → info leak (`net/metrics.c:39`).

## Checked and cleared (no defect)

So the *absence* of findings is examined, not unexamined: the **SPSC ring** memory ordering
(release/acquire on head/tail, the `dup`-fd handoff during reconnect) is correct; **signal handlers**
touch only `sig_atomic_t` and use async-signal-safe calls; **SIGPIPE is globally ignored** (so
socket/pipe writes can't kill the process); there is **no SIGCHLD handler**, so the synchronous
`waitpid` in `filter_stop` is race-free; the **telnet IAC filter** and **framing decoders** are
bounds-safe; **`selection_copy`'s** earlier heap overflow is correctly fixed with saturation
arithmetic. Two candidates were refuted: the pager `g` "blanks the whole body" claim (at most one
row; `redraw_scrollback` already guards `line_from_bottom < sb_count`), and the fuzzy `>`-vs-`>=`
re-filed as a present-day memory-safety bug (it is latent only — the feature is dead; tracked as
ZT-023).

## Honesty ledger (advertised vs. real)

The review also reconciled the docs against the source: **OSC 8 hyperlinks** (`osc8_rewrite`) and the
entire **`fastio.c` epoll/splice fast path** have zero call sites; the **fuzzy finder** and
**multi-pane** (`multi_render`) are non-functional/stubbed; **history and bookmarks are in-memory
only** (no `~/.zyterm_history` etc.); **rfc2217://** is a deliberate stub. These drove the rewritten
documentation and the [decisions](../../decisions/) ADRs.

## Roadmap synthesis (now in plans/)

Reliability: SPSC backpressure + drop accounting (the ring drops silently when full); reset stateful
decoders on reconnect; bound TX writes with a deadline; drain POLLHUP; property/fuzz tests for the
parsers; a ThreadSanitizer CI job (CI is ASan-only). Performance: wire-or-delete `fastio.c`; batch
the per-byte log writes; pool the 10k scrollback `strdup`s; damage-tracked scrollback redraw.
Features: an expect/automation engine; capture-group → CSV/JSONL export; DTR/RTS + bootloader reset
recipes; real multi-pane; `--send-file` pacing; persistent history; Modbus/NMEA decode views; an
authenticated REST/WS control plane; finishing rfc2217. Security posture: HTTP auth, drop blanket
CORS, restrict IPC socket perms, release hardening flags (and drop `-march=native` from the
distributed build), checksums/SBOM/provenance. See
[`../../plans/RELIABILITY_HARDENING.md`](../../plans/RELIABILITY_HARDENING.md) and
[`../../plans/ROADMAP.md`](../../plans/ROADMAP.md).
