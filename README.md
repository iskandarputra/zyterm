# zyterm

[![CI](https://github.com/iskandarputra/zyterm/actions/workflows/ci.yml/badge.svg)](https://github.com/iskandarputra/zyterm/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-1.5.0-green.svg)](https://github.com/iskandarputra/zyterm/releases)

> A simple serial terminal for people who work with hardware every day.

<div align="center">
  <img src="docs/assets/zyterm_vid.gif" alt="zyterm demo" width="100%">
</div>

```sh
make
./zyterm /dev/ttyUSB0 -b 115200
```

That's pretty much it. You get a live HUD, scrollback, and search out of the box. No config files to write, no dependencies to chase down.

---

## Why zyterm?

`screen`, `minicom` and `picocom` already cover the basics well. `screen`'s copy
mode has regex scrollback search; `minicom` has a searchable scrollback window.
Use either if that is what you need.

zyterm covers what they don't: a live HUD with throughput, framing and CRC
decoders, structured JSONL logs, a browser bridge, and a render path that treats
device bytes as untrusted.

What you get:

- **An input bar the device can't scroll over** — ANSI scroll regions split HUD, output and prompt into three zones. `--threaded` adds an optional reader draining into a lock-free ring, for rates the main loop can't keep up with.
- **Scrollback search** — `Ctrl+A` then `/`. Case-sensitive substring, not regex; `screen` is better at this specific job.
- **Live HUD** — Baud, parity, throughput and a sparkline. SPSC ring drops are counted here rather than swallowed.
- **Survives USB unplug** — Scrollback, search and copy keep working while the link is down. Reconnect restores the live tail without moving your view.
- **Single C binary** — libc is the only runtime dependency. Linux is the supported and CI-tested target (termios2 custom baud, inotify config reload, `/sys/class/tty` USB discovery); it may build on other Unixes but no binaries ship for them.

### How it compares

Every cell below was checked against that tool's own manual page at the version
named. The rows zyterm **loses** are listed first.

|                                              | minicom 2.10 | picocom 3.1 | screen 4.09 | tio 3.9 | zyterm 1.5.0 |
| -------------------------------------------- | :----------: | :---------: | :---------: | :-----: | :----------: |
| In your distro's repositories                |      ✓       |      ✓      |      ✓      |    ✓    |      —       |
| Terminal emulation (ANSI / VT100)            |      ✓       |      —      |      ✓      |    —    |      —       |
| Case-insensitive scrollback search           |      ✓       |      —      |      ✓      |    —    |      —       |
| A scripting language for automation          |      ✓       |      ·      |      —      |    ✓    |      ·       |
| Scrollback search, built in                  |      ✓       |      —      |      ✓      |    —    |      ✓       |
| Detach and reattach a session                |      —       |      —      |      ✓      |    —    |      ✓       |
| File transfer without external tools         |      —       |      —      |      —      |    ✓    |      ✓       |
| Reconnects when the device drops             |      —       |      —      |      —      |    ✓    |      ✓       |
| Line timestamps in logs                      |      —       |      —      |      —      |    ✓    |      ✓       |
| Live throughput HUD with sparkline           |      ·       |      —      |      —      |    ·    |      ✓       |
| Connects out to `tcp://` / `telnet://`       |      —       |      —      |      —      |    —    |      ✓       |
| Framing decoders (COBS / SLIP / HDLC) + CRC  |      —       |      —      |      —      |    —    |      ✓       |
| JSONL structured logs                        |      —       |      —      |      —      |    —    |      ✓       |
| HTTP / SSE / WebSocket browser bridge        |      —       |      —      |      —      |    —    |      ✓       |

✓ first-class · partial, or via an external tool — not supported

Notes on the close calls:

- **Scrollback search** — `screen` wins outright (`/` and `?` vi search, `n`/`N`,
  plus Emacs incremental search over its history buffer). `minicom` has `s` and
  `S` for case-sensitive and case-insensitive search in its scrollback window.
  zyterm's is a plain `strstr` (`src/tui/search.c`).
- **Terminal emulation** — `minicom` and `screen` interpret ANSI/VT100. zyterm
  deliberately does not; device escapes are neutralized by policy and only SGR
  colour is allowed back through a bounded parser
  ([INVARIANTS §6](docs/invariants/INVARIANTS.md)). Given up on purpose.
- **Scripting** — `minicom` ships `runscript` (swappable for `expect` or a
  shell); `tio` embeds Lua. zyterm has event hooks and `--filter`, which is not
  the same thing.
- **Reconnect** — `tio` does this by default too, with its own device-lookup
  strategies. zyterm is not first here.
- **Portability** — `tio` ships macOS builds and a Homebrew formula; `picocom`'s
  README says it moves to other Unix-like systems with minor changes. zyterm is
  Linux-only, on purpose.

## Quick Start

```sh
git clone https://github.com/iskandarputra/zyterm.git
cd zyterm
make
./zyterm /dev/ttyUSB0 -b 115200
```

Swap `/dev/ttyUSB0` for whatever your OS shows — `ls /dev/tty*`.

To quit, press `Ctrl+A` then `q` (or `x`).

## The Essentials

Most things are behind `Ctrl+A`. Press it to open the command menu, or use these shortcuts directly:

| Shortcut                | Action                         |
| :---------------------- | :----------------------------- |
| `Ctrl+A` then `q` / `x` | Quit                           |
| `Ctrl+A`                | Open the command menu          |
| `Ctrl+A` then `?` / `k` | Show all keybindings           |
| `Ctrl+A` then `/`       | Search through scrollback      |
| `Ctrl+A` then `l`       | Toggle logging to a file       |
| `Ctrl+A` then `o`       | Open the settings dialog       |
| `Ctrl+A` then `r`       | Force reconnect                |
| `PgUp` / `PgDn`         | Scroll through history         |
| `Ctrl+A` then `c`       | Clear the screen               |

There's more — F-key macros, hex view, bookmarks, framing decoders — and you can discover those at your own pace via `Ctrl+A ?` or in the [getting-started guide](docs/guide/getting-started.md). The full key catalogue lives in [docs/reference/KEYBINDINGS.md](docs/reference/KEYBINDINGS.md).

## A Few Handy Recipes

Capture boot output for 30 seconds, then exit:

```sh
./zyterm /dev/ttyUSB0 --dump 30 -l boot_capture.log
```

Highlight error lines so they stand out:

```sh
./zyterm /dev/ttyUSB0 --watch ERROR --watch panic
```

Replay a saved log:

```sh
./zyterm --replay boot_capture.log
```

Watch the session live in a browser (loopback only; the write routes are origin-pinned, and `--http-token` adds a bearer-token gate before you tunnel the port anywhere):

```sh
./zyterm /dev/ttyUSB0 --http 8080 --http-token "$(openssl rand -hex 16)"
```

## Installation

### Ubuntu / Debian (.deb)

Grab the latest `.deb` (amd64 or arm64) from the [Releases page](https://github.com/iskandarputra/zyterm/releases) and install:

```sh
sudo dpkg -i zyterm_*.deb
```

### Build from source

You need a C compiler and `make`. That's the whole list.

```sh
make
sudo make install
```

Or use the convenience script, which also handles formatting, linting, and packaging:

```sh
./build.sh install
```

### What about dependencies?

zyterm doesn't link against any external libraries at build time. The only runtime dependency is your system's libc.

For clipboard support on X11 desktops, zyterm quietly tries to load `libxcb.so.1` at runtime using `dlopen`. This library is already present on virtually every graphical Linux system (it ships as a runtime dependency of GTK, Qt, Mesa, etc.), so in practice clipboard "just works" without you installing anything extra. If it's not there — say, on a headless server or over SSH — zyterm falls back to OSC 52 terminal escapes or helper tools like `xclip` / `wl-copy`.

## Documentation

The docs live under [`docs/`](docs/README.md) and are organized by kind — reference (how it works now), guides (task-oriented learning), design notes, and decisions. Start at the [documentation map](docs/README.md) to find your way around.

| Resource                                             | What's in it                                    |
| :--------------------------------------------------- | :---------------------------------------------- |
| [Documentation map](docs/README.md)                  | Router for everything below                     |
| [Getting started](docs/guide/getting-started.md)     | Your first session, step by step                |
| [CLI reference](docs/reference/CLI.md)               | Every command-line flag, verified against `src` |
| [Keybindings](docs/reference/KEYBINDINGS.md)         | The full in-app key catalogue                   |
| [Architecture](docs/reference/ARCHITECTURE.md)       | How the codebase is organized                   |
| [Contributing](CONTRIBUTING.md)                      | How to send patches, style rules, testing       |
| [Security](SECURITY.md)                              | Trust boundaries and how to report issues       |

## What's Inside

Plain C11, split into nine modules under `src/` — core, serial, log, proto, render, tui, net, ext, loop — plus `main.c`. Run `make modules` if you're curious about the breakdown. The code is meant to be readable — if you want to see how something works, have a look around `src/`, and [docs/reference/ARCHITECTURE.md](docs/reference/ARCHITECTURE.md) maps the layout.

## License

MIT. See [LICENSE](LICENSE).

---

_zyterm builds on ideas from every serial terminal that came before it. If it's useful to you, that makes us happy. Stars, bug reports, and patches are always welcome._
