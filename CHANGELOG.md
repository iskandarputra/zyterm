# Changelog

All notable changes to **zyterm** are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

> **[Unreleased] discipline:** record every user-visible change here in the
> same change that introduces it — don't defer it to release time.

## [Unreleased]

### Changed
- **Device SGR colour now renders by default (ADR-0009).** Level-coloured device
  logs (`<wrn>` yellow, `<err>` red, …) show in colour out of the box instead of
  as inert `^[[1;33m` caret text. This is done through a new **bounded SGR-only
  filter**: only well-formed `CSI … m` sequences pass; OSC 52 clipboard writes,
  title injection, cursor/erase and every other escape are still neutralized, so
  the [INVARIANTS §6](docs/invariants/INVARIANTS.md) safety posture holds. Pass
  `--no-sgr` (or toggle `E` in the settings screen) for the strict deny-all mode.
  Full raw passthrough (`Ctrl+A G`) is unchanged. (`src/render/render.c`,
  `src/proto/sgr_passthrough.c`)

### Fixed
- **"SGR passthrough" was mislabeled and unsafe (ZT-029).** The flag disabled
  escape neutralization *wholesale* (it shared the `raw_ok` gate with full raw
  passthrough), so turning it on forwarded OSC/title/cursor sequences too — and
  the `sgr_filter()` meant to isolate SGR was dead, no-op code. It is now a real,
  bounded, unit-tested SGR-only parser (`sgr_feed`); the parameter whitelist
  (`0-9 ; :`) rejects private-marker sequences such as `CSI ? 1 m`, and the fixed
  parameter buffer cannot overrun. (`src/proto/sgr_passthrough.c`)

## [1.3.0] — 2026-06-13

Security-audit closeout. Closes the 28-defect source-review audit recorded
2026-06-03 — the unauthenticated `POST /tx` device-command path, hostile-device
RX driving the operator's terminal, world-reachable IPC sockets, and a long tail
of silent-failure, bounds, and memory-leak fixes — adds opt-in `--http-token`
bridge auth, makes the cold-start path wait for the device instead of aborting,
and rebuilds `docs/` into a source-verified, kind-based tree.

### Added
- **`--http-token <tok>`.** When set, the HTTP bridge requires
  `Authorization: Bearer <tok>` on the state-changing routes `POST /tx` and
  `POST /api/send` (`401` otherwise). Without it the bridge stays
  anonymous-but-origin-pinned, so the built-in web UI keeps working. (ZT-004)

### Security
- **Unauthenticated `POST /tx` → device command execution (ZT-004).** Any web
  page the operator visited could push bytes to the serial line cross-site
  (CORS simple request) or via DNS rebinding. The write routes now pin
  `Host`/`Origin` to a loopback literal and, with `--http-token`, require a
  bearer token; `cors_block` no longer advertises `POST` to `*`. (`src/net/http.c`)
- **Hostile device RX could drive the operator's terminal (ZT-003).** Device
  output was echoed verbatim (only `\r` stripped), so a device could write the
  operator's clipboard (OSC 52), set the title, or spoof the screen. ESC and
  other C0/DEL controls are now neutralized to inert `cat -v` caret notation on
  the default render path; raw escapes need the explicit `passthrough` /
  `sgr_passthrough` opt-in. (`src/render/render.c`)
- **WebSocket / SSE read the live RX stream cross-origin (ZT-013).** The `/ws`
  upgrade and `/stream` now validate `Origin`/`Host` before streaming.
- **Local IPC sockets were world-reachable (ZT-012, ZT-028).** The detach
  socket moves to `$XDG_RUNTIME_DIR`, both the detach and metrics sockets are
  created `0600`, and each verifies the peer's uid via `SO_PEERCRED`.
  (`src/net/session.c`, `src/net/metrics.c`)

### Fixed
- **Web view truncated RX bursts at 4 KiB (ZT-007).** SSE/WS broadcast now
  iterates the whole payload in ≤4096-byte segments. (`src/net/http.c`)
- **Fuzzy finder was entirely non-functional (ZT-008).** It scanned history
  from index 0 (always NULL) and keystrokes were never routed to it; it now
  scans from index 1 and `handle_stdin_chunk()` drives it. (`src/tui/fuzzy.c`,
  `src/loop/input.c`)
- **Dead WebSocket peers leaked connection slots (ZT-009, ZT-017).** A failed
  frame write is detected and the peer closed, so 16 ungraceful disconnects no
  longer DoS the bridge. (`src/net/http.c`)
- **Silent failures hardened:** log-rotation `rename`/`open` errors now warn
  (ZT-010); a non-blocking metrics scraper can't stall the loop (ZT-024); a
  flow-controlled `--webroot` transfer isn't truncated on `EAGAIN` (ZT-011); a
  signal no longer drops filter bytes (ZT-015); an OOM TX flashes "TX dropped"
  (ZT-025); a hung-up serial port is drained before reconnect (ZT-027).
- **Memory/bounds correctness:** the X11 clipboard worker NULL-checks its
  allocation (ZT-014); `osc8_rewrite`'s bound reserves the doubled URL length
  (ZT-019); `encode_cobs` reserves the true COBS worst case (ZT-021); a
  zero-length LENPFX frame dispatches without swallowing the next byte (ZT-022);
  the fuzzy selection clamp leaves NUL room (ZT-023).
- **`--http` port is range-validated** with `strtol` (1–65535) instead of an
  unchecked `atoi` (ZT-020). (`src/main.c`)
- **Early-return memory leaks consolidated (ZT-018).** `--replay`/`--attach`/
  `--diff`/`-h`/`-V`/`--profile-save` free every parse-owned field via a single
  `cleanup_ctx()` helper. (`src/main.c`)
- **Heap corruption on profile hot-reload / USB replug (ZT-001, ZT-002).**
  `c->serial.device` was sometimes a borrowed `argv` pointer yet was
  `free()`d as if heap-owned — by `profile_load()` on an inotify reload and
  by `port_rediscover()` on reconnect — causing an abort or heap corruption.
  The device string is now always heap-owned (duplicated at assignment,
  freed once on exit); this also closes the related startup leak (ZT-016).
  (`src/main.c`, `src/ext/profile.c`, `src/serial/port_discover.c`)
- **Device stranded after a failed interactive autobaud (ZT-005).** A failed
  `Ctrl+A A` left `serial.fd == -1` with no recovery path (RX silently dead,
  HUD still "connected"); it now drives the reconnect flow like `Ctrl+A r`.
  (`src/loop/input.c`)
- **UI hang on filter teardown (ZT-006).** `filter_stop()` no longer issues a
  blocking `waitpid()` from the event loop — it reaps with a short grace
  window then escalates to `SIGKILL`. (`src/ext/filter.c`)
- **UI hang on a flow-controlled serial write (ZT-026).** Outgoing writes now
  bound their `EAGAIN` retry with a progress-resetting stall deadline and
  report "TX stalled" instead of looping forever. (`src/loop/send.c`)

### Documentation
- **ADR-0007 (HTTP bridge auth model)** and **ADR-0008 (device RX escape
  default-deny)** record the *why* behind the two security decisions above;
  `INVARIANTS §6/§7` and `SECURITY.md` now reflect the enforced behaviour, and
  the man page gained a `SECURITY` section.
- **Docs rebuilt into a kind-based `docs/` tree.** Documentation is now
  organized by *kind* rather than topic: `reference/` (how it works now),
  `guide/` (task-oriented learning), `invariants/`, `decisions/` (ADRs),
  `design/`, `plans/`, `tracking/`, `ops/`, and `archive/`. A router at
  `docs/README.md` maps the layout. `CONTRIBUTING.md` and `SECURITY.md` now
  live at the repository root.

### Known issues
- A source-review audit (recorded 2026-06-03) catalogued **28 defects**, all
  now fixed on this branch — see **Security** and **Fixed** above and the full
  Resolved table in `docs/tracking/KNOWN_ISSUES.md`. The remaining
  advertised-but-dead paths (OSC 8 hyperlinks, the epoll/splice fast path,
  multi-pane) are tracked in `docs/tracking/STATUS.md`, not as defects.

## [1.2.0] — 2026-05-23

Reliability + UX audit. Hunts a long-standing deadlock that left zyterm
unresponsive after a USB unplug, finishes wiring `--threaded` (which
shipped half-implemented), opens up the disconnect popup so scrollback
keeps working, removes several dormant bug classes (HTTP slowloris,
XMODEM padding, framer state bleed between panes, ASan SEGV in the
glibc symbol pin), and brings every documented `--help` flag in line
with what the code actually does.

### Fixed
- **Ctrl+A X deadlock after USB unplug.** Three inner stdin-read loops
  (`run_reconnect_loop`, both replay loops) lacked the "short read →
  break" guard the main loop has. After the kernel queued bytes from a
  single keypress, the loop's second blocking `read()` on raw-mode stdin
  (VMIN=1, no `O_NONBLOCK`) hung indefinitely — so `zt_g_quit` was set
  but never observed. Adds the missing short-read break + EOF/EINTR
  handling at all three sites. (`src/ext/reconnect.c`,
  `src/loop/runtime.c`)
- **Disconnect popup blocking scrollback.** The centred "device link
  lost" modal owned the screen; PgUp / PgDn / mouse selection /
  Ctrl+A / search were all swallowed. The popup now auto-hides as soon
  as `sb_offset > 0` and the wait loop honours `sb_redraw`/`ui_dirty`,
  so the user can review what was on screen before the unplug, copy
  text, search, and bookmark — everything except keys that need a live
  serial fd (Ctrl+A a / A / b are politely refused). A persistent
  amber `◆ DISCONNECTED` pill in the HUD keeps the state visible even
  when the modal is hidden. Returning to bottom restores the modal.
  Reconnect doesn't yank the view away from a user mid-scroll — flashes
  `✓ reconnected` instead. (`src/ext/reconnect.c`, `src/loop/input.c`,
  `src/tui/hud.c`, `src/zt_ctx.h`)
- **Ctrl+A k / ? popup stuck on screen.** The keybind help popup set
  `popup_active = true` so the main loop's repaint pass was suppressed
  — but no key was wired to dismiss it, and Esc only cleared the input
  buffer. The footer hint "Esc to close" was a lie. Adds
  `tui.keybind_visible`; any subsequent key dismisses the popup and is
  consumed. Hint now reads "any key to close". (`src/loop/input.c`,
  `src/tui/hud.c`)
- **RX-thread fd race (`--threaded`).** The worker read directly from
  `c->serial.fd` while the main thread could close + reopen it (manual
  reconnect, autobaud, hot-unplug). Window for `read()` from a recycled
  fd → garbage bytes (or worse, the contents of another fd) into the
  ring. Worker now owns a private `dup(2)` of the fd; `rx_thread_pause`
  / `rx_thread_unpause` helpers bracket every fd swap site so the
  worker is cleanly stopped and restarted across the close+reopen.
  Memory orders on the running flag tightened to release/acquire; fd
  leak on `pthread_create` failure fixed. (`src/loop/rx_thread.c`,
  `src/serial/autobaud.c`, `include/zyterm/internal/loop.h`)
- **`--threaded` half-wired.** Previously the worker filled an SPSC
  ring that nobody drained, and *both* threads read from the same fd
  in parallel. Main loop now polls `spsc_wake_pipe[0]` and drains via
  `rx_thread_drain` when threaded mode is on, with `serial.fd` kept in
  the poll set with `events=0` for unplug detection only.
  (`src/loop/runtime.c`)
- **HTTP slowloris stall.** `accept_and_classify` busy-waited up to
  200 ms (`20 × usleep(10ms)`) for the request headers, blocking the
  whole UI/serial loop per slow client. Rewritten as
  `accept_one` (non-blocking accept) + per-connection HC_NEW slots
  pumped by `http_tick` with a 5 s header-read timeout. UI no longer
  freezes. (`src/net/http.c`)
- **HTTP `snprintf` truncation reads past stack buffer.** Eight call
  sites used the `int` return of `snprintf` directly as a `write()`
  length. On truncation the return is the would-be length, so the
  kernel read past the buffer and emitted attacker-influenced bytes to
  the network. Adds `snprintf_len` clamp and applies at all sites.
  (`src/net/http.c`)
- **XMODEM receiver wrote trailing 0x1A pad bytes.** The standard
  XMODEM block size is 128 and the last block is zero-filled with
  0x1A (CP/M SUB). The receiver wrote them verbatim, so files always
  ended with garbage and the size was rounded up to a 128-byte
  boundary. Now buffers one block ahead and trims trailing 0x1A on
  EOT. `fwrite` and `fclose` return values are checked so a full disk
  surfaces as an error instead of a silently truncated file.
  (`src/proto/xmodem.c`)
- **ZMODEM relay could hang on a wedged child.** Serial-side
  EOF/POLLHUP wasn't an exit condition for the relay loop, and the
  final `waitpid` was unbounded. Now exits on either side closing;
  child is reaped with a bounded SIGTERM → SIGKILL fallback (~4 s
  worst case). (`src/proto/xmodem.c`)
- **Framer file-static state.** `feed_cobs` and `feed_len16` kept
  decoder state in file-statics — `framing_reset()` couldn't clear it
  on mode change, and the multi-pane code shared the same statics
  across panes (frames bled between devices). State moved into
  `c->proto`. Defensive `wr > rd` guard added against malformed COBS.
  (`src/proto/framing.c`, `src/zt_ctx.h`)
- **F-key parser dropped trailing bytes.** When a terminal coalesced
  an F-key escape sequence with the next keystroke into a single
  `read()`, the trailing key was silently lost. `fkey_index_consume`
  now returns the prefix length so `handle_escape_seq` can re-dispatch
  the remainder. (`src/proto/macros.c`, `src/loop/input.c`)
- **`sig_crash` longjmp from fault signals was unsafe.** `siglongjmp`
  out of SIGSEGV / SIGBUS / SIGFPE-fault back into running host code
  is C-language UB — the heap is suspect and the next `malloc`/`free`
  double-faults. Restricted embed-host recovery to SIGABRT/SIGFPE.
  Memory-fault signals now restore the terminal and re-raise so the
  process dies cleanly with a known exit status. (`src/core/core.c`)
- **`profile_watch_tick` could walk past the inotify event buffer.**
  Defensive bounds check and `strnlen` before `strcmp` on `ev->name`.
  (`src/ext/profile_watch.c`)
- **`selection_copy` 256 KB cap math could underflow.** The
  saturating arithmetic was fragile against a future change that
  allowed `len + 1 > kMaxSel`. Hardened with explicit `room` calc.
  (`src/log/scrollback.c`)
- **`reconnect_attempt` recursion via Ctrl+A r during disconnect.**
  Pressing Ctrl+A r from inside the disconnect popup would re-enter
  `run_reconnect_loop`, leaking the inner fd on success. Now
  short-circuits to a `set_flash("retrying…")` and lets the existing
  wait-loop tick do the actual retry. (`src/loop/input.c`)
- **`--profile-save NAME` was wired to `session_detach`.** The handler
  stored the name in `net.session_name` instead of calling
  `profile_save` — so the documented "snapshot and exit" instead
  silently tried to detach as a session named NAME. Now actually saves
  to `~/.config/zyterm/NAME.conf` and exits. (`src/main.c`)
- **`Ctrl+A ?` did nothing.** The connect banner and `--help` both
  advertised `Ctrl+A ?` → help but no handler existed; `?` now aliases
  `k`. (`src/loop/input.c`)
- **Compact key list in `--help` lied.** The one-liner
  `(q x p e c h t b s f r / a ?)` listed `?` (didn't exist) and omitted
  ~14 keys that did. Replaced with a pointer to the actual help popup
  plus six high-traffic shortcuts. (`src/main.c`)
- **ASan SEGV in `strtol` from `atoi("230400")`.** The glibc symbol
  pin (`.symver __isoc23_strtol → strtol@GLIBC_2.2.5` — needed so
  binaries built on 24.04 still run on 22.04) bypassed the
  AddressSanitizer / ThreadSanitizer / MemorySanitizer interceptors,
  which are keyed on the modern `GLIBC_2.38` symbol. SEGV on the
  fabricated pointer `0x303034303332` (ASCII bytes of the input
  string). The pin is now skipped under any sanitizer
  (`__SANITIZE_ADDRESS__` / `__SANITIZE_THREAD__` / clang
  `__has_feature`). Sanitizer builds are dev-only and don't need
  old-glibc portability. (`src/zt_internal.h`)
- **`--autobaud` was broken.** The flag set `serial.baud = 0` as a
  sentinel, but `setup_serial` would then call `set_custom_baud(fd, 0)`
  and `zt_die`. Now opens at 115200 first, then runs `autobaud_probe`
  to discover the right rate. (`src/main.c`)
- **`--epoll` was a no-op stub.** `fastio_init` was never called from
  anywhere. Flag removed from the parser. (`src/main.c`)

### Added
- **Wired-up `--threaded` mode.** Documented in `--help` ADVANCED I/O.
  Worker thread drains serial into a 1 MiB SPSC ring; main thread
  consumes via wake-pipe poll. Designed to reduce UART-latency jitter
  at ≥ 1 Mbaud. (`src/loop/rx_thread.c`, `src/loop/runtime.c`)
- **`--help` honesty pass.** Surfaced previously-hidden flags in five
  new sections: `LOGGING & CAPTURE` (added `--log-format`,
  `--mute-dbg`, `--mute-inf`), `FRAMING & CRC` (`--frame`, `--crc`),
  `ADVANCED I/O` (`--threaded`, `--autobaud`), `HTTP & STREAMING`
  (`--http`, `--webroot`, `--http-cors`, `--metrics`),
  `SESSIONS & INTEGRATION` (`--detach`, `--attach`, `--filter`,
  `--diff`), `CLIPBOARD` (`--osc52`, `--no-osc52`). Three new
  examples in the footer. (`src/main.c`)
- **HUD `◆ DISCONNECTED` pill** while reconnect loop runs.
  (`src/tui/hud.c`)
- **`zt_cached_hhmmss`** helper. The render path's per-line
  `localtime_r` + per-field `snprintf` was hot at ≥ 500k baud / 1000+
  lines/sec; now cached on the second boundary. (`src/core/core.c`)

### Changed
- **CI matrix is Linux-only.** macOS dropped — zyterm targets Linux
  serial surfaces (termios2, inotify, `/sys/class/tty` USB discovery)
  and we don't ship macOS binaries. Both gcc and clang are still in
  matrix, both with and without ASan+UBSan. Per-job (`timeout-minutes:
  15`) and per-step timeouts added; `timeout(1)` belt-and-braces
  around the actual test/smoke invocations.
  (`.github/workflows/ci.yml`)
- **GitHub Actions bumped off Node 20.** `actions/checkout@v4 → v5`,
  `actions/upload-artifact@v4 → v5`, `actions/download-artifact@v4 →
  v5`. The transitive Node-20 dep via `awalsh128/cache-apt-pkgs-action`
  was removed by inlining `apt-get install`.
  (`.github/workflows/ci.yml`, `.github/workflows/release.yml`)
- **Whole-tree clang-format pass.** The format-check guard had been
  drifting on `main` for a while; now clean against clang-format-18+.

## [1.1.3] — 2026-04-21

Patch release to align the tagged release with `main` after a follow-up
warning cleanup.

### Fixed
- **USB port discovery build warnings** — `src/serial/port_discover.c`
  now bounds-checks sysfs attribute path construction for `idVendor`
  and `idProduct`, which removes fortified `snprintf` truncation
  warnings on strict Linux builds without changing runtime behaviour.

## [1.1.2] — 2026-04-21

Linux feature push: hooks, hot-reload, asciinema recording, TCP/Telnet,
USB hot-plug discovery, line-ending mapping. Plus a deep multi-chapter
tutorial book and a real test suite (208 unit + 11 e2e + 20 pty).

### Added
- **Event hooks** — three new flags fire shell actions in response to
  session events: `--on-match '/REGEX/=ACTION'` (POSIX ERE, matched
  per line after reassembly), `--on-connect ACTION`, and
  `--on-disconnect ACTION`. Each flag can be passed multiple times.
  Actions run under `/bin/sh -c` with stdin redirected to `/dev/null`,
  so `>`, `|`, `&&`, environment substitution and shell built-ins all
  work. An `ACTION` prefixed with `send:` is instead injected back
  down the TX path (with the usual `\n`/`\r`/`\xHH` escape expansion),
  enabling match→respond automation like `--on-match '/login:/=send:root\n'`.
  Child processes are reaped non-blocking (`waitpid(WNOHANG)`) once
  per main-loop tick; up to 32 simultaneous children tracked. Each
  hook is individually rate-limited to 100 ms between fires to tame
  pathological chatter. Four env vars are set in the child:
  `ZYTERM_PORT`, `ZYTERM_BAUD`, `ZYTERM_LINE` (full matched line),
  `ZYTERM_MATCH` (the raw pattern). Works in both interactive and
  `--dump` modes.
- **Config hot-reload** — `--profile NAME` now auto-watches the
  resolved profile file (`$XDG_CONFIG_HOME/zyterm/NAME.conf` or
  `~/.config/zyterm/NAME.conf`) via Linux **inotify** and re-applies
  the profile on every edit. Watching the *parent directory* (not the
  file itself) means atomic-rename writers — vim, neovim, helix,
  vscode, `sponge`, `cp/mv` — survive the inode swap. A 200 ms debounce
  coalesces editors that fire CREATE + CLOSE_WRITE + MOVED_TO in
  rapid succession on `:w`. Runtime-safe keys (macros, watch
  patterns, line endings, log level) take effect instantly; non-
  runtime-safe keys (baud, device, framing) are loaded into context
  and surfaced via a scrollback notice ("take effect on next
  reconnect"). Cheap when not in use: zero-overhead non-blocking
  drain on the inotify fd in the existing main-loop tick block, no
  extra threads.
- **Asciinema cast v2 session recording** — `--rec session.cast` taps
  every byte the renderer is about to emit (single static callback
  registered on the shared stdout output buffer in `core.c`) and
  writes a standards-conformant `.cast` file: a JSON header line
  (`{"version":2,"width":W,"height":H,"timestamp":T,"env":{TERM,SHELL},
  "title":"zyterm <ver>"}`) followed by `[t_seconds,"o","data"]`
  events with `\b\f\n\r\t`, backslash, double-quote, and `\u00XX`
  control-byte escaping. Plays back with `asciinema play file.cast`,
  embeds in the asciinema web player, and round-trips through `agg`
  to GIF/SVG. Works in both interactive mode (records the rendered
  TUI: scrollback, HUD, colour passthrough) and `--dump` mode
  (records raw RX from a serial / TCP transport — perfect for
  shareable bug repros). File is line-buffered so a Ctrl+C still
  leaves a parseable artefact on disk.
- **Man page and shell completions.** `docs/zyterm.1` (groff), plus
  `contrib/completions/{zyterm.bash,_zyterm,zyterm.fish}`. `make install`
  now lays them down under `$MANDIR`, `$BASHCOMPDIR`, `$ZSHCOMPDIR`,
  `$FISHCOMPDIR` (all overridable). Completions know the option list,
  baud-rate / parity / EOL-mode value sets, common USB VID:PID examples,
  and offer `tcp://` / `telnet://` / `rfc2217://` for the positional
  device argument.
- **TCP / Telnet transport** — `<device>` may now be a network URL:
  `tcp://host:port`, `telnet://host:port`, or `rfc2217://host:port`
  (the last is stubbed with an actionable error pointing at the
  ser2net raw-mode workaround). Lets zyterm talk to remote serial
  servers (ser2net, esp-link, USB-IP, SSH-tunneled tty) the same way
  it talks to a local `/dev/ttyUSB0`. TCP defaults to `TCP_NODELAY`
  + `SO_KEEPALIVE` + non-blocking; `telnet://` adds passive IAC
  negotiation (escape outgoing `0xFF`, strip incoming
  WILL/WONT/DO/DONT and sub-negotiations). Baud / parity flags are
  silently ignored on socket transports.
- **`--port-glob` / `--match-vid-pid`** — USB hot-plug-aware port
  discovery. `--port-glob "/dev/ttyUSB*"` re-resolves the device path
  on every reconnect attempt, so an FT232 / CH340 that comes back as
  a different `ttyUSBn` after a replug is found again automatically.
  `--match-vid-pid 0403:6001` filters discovered ports by USB
  vendor:product (sysfs walk; no libudev dependency). Either flag
  alone makes the positional `<device>` argument optional — handy for
  `zyterm --match-vid-pid 1a86:7523` to "open whatever CH340 is
  plugged in right now".
- **`--map-out` / `--map-in`** — line-ending translation between host and
  device. Modes: `none | cr | lf | crlf | cr-crlf | lf-crlf`. Streaming,
  stateful (CRLF coalescing survives chunk boundaries), wired through
  both TX (`trickle_send` / `direct_send`) and RX (`rx_ingest`) paths,
  so logging, JSONL, HTTP/SSE and the renderer all see the same
  normalised stream. Persisted by `--profile-save`. Resolves the
  most-reported minicom-parity gap (Windows-firmware devices that
  expect CRLF, RTOS shells that expect bare CR).
- `CHANGELOG.md` (this file) and `.github/` issue / PR templates.
- **Modern, color-aware `--help` layout** — banner, section headers,
  per-row aligned columns, ANSI palette that auto-disables on
  `NO_COLOR`, `TERM=dumb`, or non-TTY stderr.
- **Deep tutorial book** under `docs/tutorial/` (10 chapters, ~1500
  lines): install, first session, logging, profiles, hooks, recording,
  recipes, troubleshooting, full reference. Cross-linked from
  `README.md` and `docs/USER_GUIDE.md`.
- **USER_GUIDE first-5-minutes** quickstart and new-flag rows for
  `--rec`, `--profile`, `--on-*`, `--port-glob`, `--match-vid-pid`,
  `--map-out`, `--map-in`.
- **README comparison table** vs minicom / picocom / screen / tio /
  PuTTY.

### Fixed
- `--on-match '/REGEX/=ACTION'` parser now scans forward for the `/=`
  separator instead of using `strrchr`, so `ACTION` values that
  contain `/` (paths, sed expressions, etc.) are no longer mis-split.
- `docs/USER_GUIDE.md` profile example now uses real, parser-supported
  keys. The previous example listed `log-dir`, `watch`, `macro` —
  the parser silently ignored those keys, so users editing the
  example saw no effect. Real keys are documented in
  `docs/tutorial/04-profiles.md`.
- Removed phantom `--list` flag reference from `USER_GUIDE.md` (no
  such flag exists; use `ls /dev/tty*` + `dmesg`).

### Tests
- **208 unit tests** (`tests/unit/test_subsystems.c`) covering the
  hooks parser, asciinema cast writer, profile inotify watcher, and
  the existing CRC / log-level / OSC8 / sparkline subsystems.
- **11 e2e tests** (`tests/integration/test_e2e_hooks.c`) drive the
  real `./zyterm` binary against a TCP listener with `fork+exec`
  hook actions and a 100 ms rate-limit assertion.
- **20 pty tests** (`tests/pty/pty_harness.c`) — pre-existing.
- Total **239/239 passing**. Run with `make -C tests run`.

## [1.1.1] — 2025-01

### Fixed
- **Scrollback selection alignment** — mouse selections in scrollback
  now line up correctly when the `--ts` timestamp gutter is hidden,
  matching the visible layout.

### Changed
- **libxcb is no longer a build dependency.** The X11 clipboard owner
  is loaded at runtime via `dlopen("libxcb.so.1")`, so packages can
  ship a single `.deb` that runs on minimal hosts (no X) and
  graphical hosts alike. `--no-osc52` users are unaffected.

### Released
- Debian package: `zyterm_1.1.1_amd64.deb`
  ([GitHub Release](https://github.com/iskandarputra/zyterm/releases/tag/v1.1.1))

## [1.2.0-dev → 1.1.1] — internal

Pre-release work that became part of 1.1.1:

### Added
- HTTP/SSE/WS bridge (`--http`, `--webroot`, `--http-cors`)
- Detach / attach sessions (`--detach`, `--attach`)
- Saved profiles (`--profile`, `--profile-save`)
- OSC 52 selection-to-clipboard (on by default; `--no-osc52` to disable)
- OSC 8 hyperlink rendering
- ANSI SGR pass-through for device-emitted colour
- KGDB / gdbserver raw passthrough mode
- Frame decoders: COBS, SLIP, HDLC, length-prefix (`--frame`)
- CRC modes: CCITT, IBM, CRC32 (`--crc`)
- Filter subprocess (`--filter "jq ."`)
- Metrics socket (`--metrics`)
- JSONL log format (`--log-format json`)
- Bookmarks, log-level mute, fuzzy command palette
- Diff mode (`--diff a.log b.log`)
- Threaded RX (`--threaded`) and epoll fast path (`--epoll`)
- Autobaud (`--autobaud`)
- Replay (`--replay`, `--replay-speed`)

## [1.0.0] — 2025-01

Initial public release.

### Core
- termios2 / `BOTHER` for arbitrary baud rates (Linux)
- Per-byte trickle send with `--flow n|r|x`
- Auto-reconnect on USB hot-unplug (default ON; `--no-reconnect` to
  exit instead)
- Hex view (`--hex`), local echo (`-e`), timestamp gutter (`--ts`)
- Scrollback with search (`Ctrl+A /`), watch patterns (`--watch`)
- F1..F12 macros (`--macro F<n>=<str>`)
- Headless capture (`--dump <sec>`)
- Log rotation (`--log-max-kb`)

[Unreleased]: https://github.com/iskandarputra/zyterm/compare/v1.3.0...HEAD
[1.3.0]: https://github.com/iskandarputra/zyterm/releases/tag/v1.3.0
[1.2.0]: https://github.com/iskandarputra/zyterm/releases/tag/v1.2.0
[1.1.3]: https://github.com/iskandarputra/zyterm/releases/tag/v1.1.3
[1.1.2]: https://github.com/iskandarputra/zyterm/releases/tag/v1.1.2
[1.1.1]: https://github.com/iskandarputra/zyterm/releases/tag/v1.1.1
[1.0.0]: https://github.com/iskandarputra/zyterm/releases/tag/v1.0.0
