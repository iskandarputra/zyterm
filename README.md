# zyterm

[![CI](https://github.com/iskandarputra/zyterm/actions/workflows/ci.yml/badge.svg)](https://github.com/iskandarputra/zyterm/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-1.1.0-green.svg)](https://github.com/iskandarputra/zyterm/releases)

> A friendly serial terminal for talking to microcontrollers and embedded
> boards. One small binary. No dependencies. Made to be pleasant to use.

<div align="center">
  <img src="docs/assets/zyterm_vid.gif" alt="zyterm demo" width="100%">
</div>

```sh
make
./zyterm /dev/ttyUSB0 -b 115200
```

That's it. You're talking to your board.

```
┌─────────────────────────────────────────────────────────────────────┐
│ /dev/ttyUSB0  115200 8N1   ↓ 1.2 MB   ↑  4 KB   ▁▂▄▇█▇▄▂           │
│─────────────────────────────────────────────────────────────────────│
│ <inf> wifi: connected to home-2g                                    │
│ <wrn> sensor: i2c retry 1/3                                         │
│ <err> nvs: write failed -28                                         │
│─────────────────────────────────────────────────────────────────────│
│ > reboot_                                                           │
└─────────────────────────────────────────────────────────────────────┘
```

## What is this?

A _serial terminal_ is the program you use to read messages from, and
type commands into, a USB-attached board. An ESP32. A Raspberry Pi
Pico. An Arduino. An STM32 dev kit. Anything with a UART.

zyterm is one of many such tools. You may already know `screen`,
`minicom`, `picocom`, or PlatformIO's monitor. They all work well.
zyterm exists because we wanted one that is:

- friendlier out of the box (status line, scrollback, search built in),
- thoughtful about log capture instead of treating it as an afterthought,
- responsive even during high-volume logging (input bar stays interactive via ANSI scrolling regions while megabytes of data scroll through),
- a single small binary you can drop on any Linux or macOS box,
- easy to build: `make` and you're done.

If your current tool works for you, please keep using it. If you're
curious, read on.

## Quick start

```sh
git clone https://github.com/iskandarputra/zyterm.git
cd zyterm
make
./zyterm /dev/ttyUSB0 -b 115200
```

Plug in your board, swap `/dev/ttyUSB0` for whatever your OS shows
(`ls /dev/tty*` on Linux, `ls /dev/cu.*` on macOS), and pick the baud
rate your firmware uses. Common ones are 9600, 115200, 460800, and 921600.

When you're done, press `Ctrl+A` then `q` to quit.

## The keys you'll use most

| Key               | What it does                                |
| ----------------- | ------------------------------------------- |
| `Ctrl+A` then `q` | Quit                                        |
| `Ctrl+A`           | Show the command menu (all shortcuts)       |
| `Ctrl+A` then `l` | Start or stop logging to a file             |
| `Ctrl+A` then `/` | Search the scrollback                       |
| `Ctrl+A` then `o` | Settings dialog (serial, display, logging)  |
| `Ctrl+A` then `k` | Keybindings reference                       |
| PgUp / PgDn       | Scroll back through what your board printed |
| Up / Down         | Step through commands you've typed before   |
| `Ctrl+L`          | Clear the screen                            |

There is more (macros on F1 to F12, hex view, settings dialog, fuzzy
finder, multi-pane, and so on). The full list lives in
[docs/USER_GUIDE.md](docs/USER_GUIDE.md).

## Common recipes

Capture a boot log to a file:

```sh
./zyterm /dev/ttyUSB0 -b 115200 -l boot.log
```

Watch silently for 30 seconds and save (no UI):

```sh
./zyterm /dev/ttyUSB0 --dump 30 -l capture.log
```

Replay a saved log later:

```sh
./zyterm --replay boot.log
```

Highlight error lines so they stand out:

```sh
./zyterm /dev/ttyUSB0 --watch ERROR --watch panic
```

## Install

### Build from source

Build dependencies are tiny: a C compiler, `make`, and `pthread`.
Most systems already have all three.

```sh
make            # build ./zyterm
make test       # optional: run the test suite
sudo make install   # /usr/local/bin/zyterm
```

### Install via .deb (Debian/Ubuntu)

A helper script builds a `.deb` package you can install with `dpkg`:

```sh
./build.sh deb            # produces releases/zyterm_<version>_<arch>.deb
sudo dpkg -i releases/zyterm_*.deb

# or build + install in one step:
./build.sh install
```

### Supported platforms

Works on Linux, macOS, and FreeBSD. Windows is not supported yet.

## Build helper

The `build.sh` script provides a single entry point for all development
tasks:

```sh
./build.sh build          # compile release binary
./build.sh debug          # compile debug binary (-O0 -g3)
./build.sh format         # auto-format all source files (clang-format)
./build.sh format-check   # check formatting without modifying (for CI)
./build.sh lint           # run cppcheck static analysis
./build.sh test           # build + run the full test suite
./build.sh deb            # build + package as .deb
./build.sh install        # build + package .deb + install via dpkg
./build.sh clean          # remove build artifacts + packages
./build.sh all            # format → lint → build → test → deb
```

## Documentation

| Doc                                          | When to read                                     |
| -------------------------------------------- | ------------------------------------------------ |
| [docs/USER_GUIDE.md](docs/USER_GUIDE.md)     | Every flag, every key, examples and recipes.     |
| [docs/API.md](docs/API.md)                   | If you want to embed zyterm in your own program. |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | If you're curious how it's built inside.         |
| [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) | If you'd like to send a patch.                   |

## What's inside

About 8.8k lines of plain C11. Ten small modules:

```
src/
├── core/    foundations: memory, ring buffer, terminal raw-mode
├── serial/  open the port, read/write bytes, autobaud
├── log/     capture to disk, JSON lines, rotating scrollback
├── proto/   framing (COBS/SLIP/HDLC), XMODEM, hyperlinks,
│             native X11 clipboard owner
├── render/  draw the screen
├── tui/     HUD, glassmorphism dialogs, settings menu, search,
│             pager, fuzzy finder
├── net/     optional HTTP bridge, metrics, attach/detach sessions
├── ext/     bookmarks, diff, filter, multi-pane, profiles
└── loop/    the main read/write loop and reader thread
```

Single binary, around 184 KB. Runtime dependencies: libc, pthread.
Build-time optional: libxcb + libxcb-xfixes for native X11 clipboard
support (zero hard dependency; falls back to OSC 52 + helper binaries
or pure OSC 52 on systems without xcb).

## License

MIT. See [LICENSE](LICENSE). Use it however you like.

## Contributing

Contributions are welcome! See [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md)
for style guidelines, the dependency rule, and how to submit patches.

## Thanks

zyterm stands on the shoulders of every serial-terminal tool that came
before it. If you'd like to contribute back, that would be lovely.
