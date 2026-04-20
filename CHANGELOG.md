# Changelog

All notable changes to **zyterm** are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/iskandarputra/zyterm/compare/v1.1.1...HEAD
[1.1.1]: https://github.com/iskandarputra/zyterm/releases/tag/v1.1.1
[1.0.0]: https://github.com/iskandarputra/zyterm/releases/tag/v1.0.0
