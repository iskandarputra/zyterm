# zyterm

[![CI](https://github.com/iskandarputra/zyterm/actions/workflows/ci.yml/badge.svg)](https://github.com/iskandarputra/zyterm/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-1.2.0-green.svg)](https://github.com/iskandarputra/zyterm/releases)

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

If you work with microcontrollers or embedded boards, you've probably spent a lot of time in `screen`, `minicom`, or `picocom`. They're solid tools and they've served the community well for decades. But they can feel a little dated sometimes — no built-in scrollback, no easy way to search past output, and they can get sluggish when a board dumps a wall of text.

We wanted something that felt a bit more comfortable for daily use, so we built zyterm. It's nothing revolutionary — just a serial terminal that tries to stay out of your way while giving you a few nice things:

- **Stays responsive under load** — Uses ANSI scrolling regions and an optional reader thread (`--threaded`), so your input bar doesn't freeze when the board is spewing boot logs. It handles high baud rates reasonably well, though your mileage may vary depending on the USB adapter.
- **Built-in search & scrollback** — Press `Ctrl+A` then `/` to search through what your board has printed. Handy when you're looking for that one error buried in a thousand lines.
- **Live HUD** — A small status bar showing baud rate, parity, throughput, and a sparkline. Nothing fancy, but useful at a glance.
- **Survives USB unplug** — Pull the adapter out mid-session and zyterm shows a disconnect dialog instead of crashing. Scrollback, search, and copy still work while the link is down; reconnecting restores the live tail without disturbing what you were reading.
- **Small and self-contained** — It's a single C binary. No Python, no Node, no runtime dependencies beyond your system's libc. Linux is the supported and CI-tested target (termios2 custom baud, inotify config reload, `/sys/class/tty` USB discovery); it may build on other Unixes but we don't ship binaries for them.

### How it compares

A rough feature snapshot against the usual suspects on Linux. None of these
tools are bad — pick whatever fits your workflow:

| Feature                                            | minicom | picocom | screen | tio | zyterm |
| -------------------------------------------------- | :-----: | :-----: | :----: | :-: | :----: |
| Scrollback + in-stream search                      |    —    |    —    |   ·    |  ·  |   ✓    |
| Built-in HUD (baud, throughput, sparkline)         |    —    |    —    |   —    |  —  |   ✓    |
| USB hot-plug rediscovery (`--port-glob` / VID:PID) |    —    |    —    |   —    |  ✓  |   ✓    |
| Network transports (`tcp://`, `telnet://`)         |    —    |    —    |   —    |  ·  |   ✓    |
| Line-ending translation (CRLF / LF / CR / mixed)   |    ✓    |    ✓    |   —    |  ✓  |   ✓    |
| Timestamped + JSONL logging                        |    ·    |    ·    |   —    |  ✓  |   ✓    |
| HTTP/SSE bridge for browsers                       |    —    |    —    |   —    |  —  |   ✓    |
| Single static C binary, no runtime deps            |    ✓    |    ✓    |   ✓    |  ✓  |   ✓    |

✓ first-class · partial / via add-on — not supported

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
