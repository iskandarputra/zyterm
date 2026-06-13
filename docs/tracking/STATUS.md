# Status

The non-defect work board: what has shipped, what is partial, and what is deliberately deferred.
Defects do not live here — confirmed bugs are in [KNOWN_ISSUES.md](KNOWN_ISSUES.md). This board
tracks feature/work state, not correctness.

**Legend:** ✅ shipped · 🟡 partial · ⏸ deferred · 📋 planned.

## Open / backlog

This board records *state*, not the detailed work. Where the work itself lives:

- Feature direction and future epics → [ROADMAP.md](../plans/ROADMAP.md).
- Fix order, perf, testing, and security-posture hardening → [RELIABILITY_HARDENING.md](../plans/RELIABILITY_HARDENING.md).
- Confirmed defects (with severity, location, and fix direction) → [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## Shipped

The real, verified-working feature set of zyterm 1.3.0. Each line is grounded in `src/`; anything
absent or marked deferred below is not a working feature.

| Area | State | Notes |
|------|-------|-------|
| Framing | ✅ | `raw`, `cobs`, `slip`, `hdlc`, `lenpfx` via `--frame` (`src/proto/framing.c`); cycle live with `Ctrl+A F`. |
| CRC | ✅ | `none`, `ccitt`, `ibm`, `crc32` via `--crc`; cycle live with `Ctrl+A K`. |
| File transfer | ✅ | XMODEM-CRC, YMODEM, ZMODEM (via `lrzsz`) — `src/proto/xmodem.c`. |
| Autobaud | ✅ | `--autobaud` flag and `Ctrl+A A` probe. |
| Reconnect & discovery | ✅ | On by default ([ADR-0004](../decisions/0004-reconnect-on-by-default.md)); `--no-reconnect`, `--port-glob`, `--match-vid-pid` (`src/serial/port_discover.c`). |
| Threaded reader | ✅ | `--threaded` SPSC ring reader (`src/loop/rx_thread.c`). |
| Line-ending mapping | ✅ | `--map-in` / `--map-out` (`none\|cr\|lf\|crlf\|cr-crlf\|lf-crlf`) — `src/proto/line_endings.c`. |
| Transports | ✅ | Local serial, `tcp://`, `telnet://` (`src/serial/transport.c`). |
| HTTP / SSE / WS bridge | ✅ | `--http`, `--webroot`, `--http-cors` (`src/net/http.c`). See security caveats below. |
| Prometheus metrics | ✅ | `--metrics <path>` UNIX socket (`src/net/metrics.c`). |
| Detach / attach sessions | ✅ | `--detach <name>` / `--attach <name>` (`src/net/session.c`). |
| Output filter | ✅ | `--filter <cmd>` pipes RX through an external command (`src/ext/filter.c`). |
| Event hooks | ✅ | `--on-match /RE/=CMD`, `--on-connect`, `--on-disconnect`; `send:` action injects TX (`src/ext/hooks.c`). |
| Profiles | ✅ | `--profile` / `--profile-save`, persisted at `~/.config/zyterm/<name>.conf`, inotify hot-reload (`src/ext/profile.c`, `profile_watch.c`). |
| Recording (asciinema) | ✅ | `--rec <file>` cast capture (`src/log/record_cast.c`). |
| Replay | ✅ | `--replay <file>` / `--replay-speed <x>`. |
| Logging | ✅ | `--log` with `text` / `json` (NDJSON) / `raw` formats + size rotation (`src/log/logio.c`, `log_json.c`). |
| Scrollback & search | ✅ | Ring buffer + regex search + bookmarks (in-memory) — `src/log/scrollback`, `src/tui/search.c`, `src/ext/bookmarks.c`. |
| Pager | ✅ | `src/tui/pager.c`. |
| Settings menu | ✅ | `Ctrl+A o` — 4 pages (serial / screen / keyboard / logging). |
| Clipboard | ✅ | In-app mouse selection, OSC 52, and native xcb clipboard (runtime `dlopen("libxcb.so.1")`) — `src/proto/clipboard.c`. |
| SGR passthrough | ✅ | `src/proto/sgr_passthrough.c`. |
| Transparent (KGDB/raw) passthrough | ✅ | `Ctrl+A G` — wired transparent relay: raw stdin↔device, TUI/scrollback/SGR-filter suspended, exit with `~.`. `src/proto/passthrough.c` + `src/loop/runtime.c`. |
| Diff | ✅ | `--diff <a> <b>` (`src/ext/diff.c`). |
| Embedding API | ✅ | 7 exported symbols, `build/zyterm_embed.a` — see [EMBEDDING.md](../reference/EMBEDDING.md). |

## Deferred / stubbed

These are not working features. They are recorded here so they are never mistaken for shipped
functionality and never re-advertised in the README. Latent bugs in the dead paths are tracked in
[KNOWN_ISSUES.md](KNOWN_ISSUES.md).

| Item | State | Why / where |
|------|-------|-------------|
| epoll/splice fast path | ⏸ | `src/serial/fastio.c` is entirely unwired (no call site); the runtime uses `poll(2)` and the `--epoll` flag was removed in 1.2.0 → [ADR-0003](../decisions/0003-epoll-splice-fastpath-deferred.md). |
| `rfc2217://` transport | ⏸ | Intentional stub: `transport_open()` `zt_die`s "NYI; use ser2net raw + tcp://" (`src/serial/transport.c`) → [ADR-0005](../decisions/0005-rfc2217-deferred.md). |
| Multi-pane | 🟡 | `multi_render()` is a no-op stub (`src/ext/multi.c`); not wired, not keybound, not discoverable. Real multi-pane is on the [ROADMAP](../plans/ROADMAP.md). |
| In-memory history & bookmarks | ⏸ | History and bookmarks are in-memory only and lost on exit; no `~/.zyterm_history` / `~/.zyterm/bookmarks` file is written → [ADR-0006](../decisions/0006-in-memory-history-and-bookmarks.md). |

_Last updated: 2026-06-03._
