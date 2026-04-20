# Changelog

All notable changes to **zyterm** are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
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
