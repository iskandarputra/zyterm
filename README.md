# zyterm

[![CI](https://github.com/iskandarputra/zyterm/actions/workflows/ci.yml/badge.svg)](https://github.com/iskandarputra/zyterm/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-1.1.0-green.svg)](https://github.com/iskandarputra/zyterm/releases)

> Serial terminals have felt a bit stuck in the past. We decided it was time to build something a little more modern.

<div align="center">
  <img src="docs/assets/zyterm_vid.gif" alt="zyterm demo" width="100%">
</div>

```sh
make
./zyterm /dev/ttyUSB0 -b 115200
```
That's it! You are instantly connected to your board with a live HUD, scrollback, and built-in search. No complicated config files, and no cryptic shortcuts to memorize.

---

## Why zyterm?

If you build hardware, you probably spend countless hours staring at a serial monitor. While classics like `screen`, `minicom`, and `picocom` have served us well, they haven't changed much in decades. They usually lack native scrollback, they can freeze up when your board spews errors, and searching through past logs means you have to dump everything to a file and switch over to `grep`.

We built **zyterm** from the ground up to be the kind of serial terminal you actually look forward to using.

- 🚀 **Blazing Fast**: zyterm uses ANSI scrolling regions and a dedicated reader thread. If your board panics and spews megabytes of text, zyterm won't lock up or drop characters. Your input bar stays perfectly responsive.
- 🔍 **Built-in Search & Scrollback**: Just press `Ctrl+A` then `/` to instantly fuzzy-search through everything your board has printed.
- 📊 **Live HUD**: The bottom of your screen shows exactly what is happening: your baud rate, parity, real-time RX/TX throughput, and even a live sparkline graph.
- 🪶 **Zero Dependencies**: It is just a single 184 KB binary written in clean C11. There is no Python, no Node, and no giant runtimes. Just drop it on any Linux or macOS machine and it works right away.

## Quick Start

Getting started takes less than 10 seconds:

```sh
git clone https://github.com/iskandarputra/zyterm.git
cd zyterm
make
./zyterm /dev/ttyUSB0 -b 115200
```

*(Just swap `/dev/ttyUSB0` for whatever your OS shows. You can use `ls /dev/tty*` on Linux, or `ls /dev/cu.*` on macOS).*

**To safely quit**, simply press `Ctrl+A` then `q`.

## ⚡ The Essentials

zyterm is packed with features, and they are all accessible via a simple command menu. Press **`Ctrl+A`** to open the menu, or use the quick keys below:

| Shortcut | Action |
| :--- | :--- |
| `Ctrl+A` then `q` | **Quit** safely |
| `Ctrl+A` | Open the main command menu |
| `Ctrl+A` then `/` | **Search** through the scrollback |
| `Ctrl+A` then `l` | Start or stop logging directly to a file |
| `Ctrl+A` then `o` | Open the Settings dialog (baud, parity, UI) |
| `PgUp` / `PgDn` | Scroll back through history |
| `Ctrl+L` | Clear the screen |

Need a bit more power? zyterm also supports F1-F12 macros, hex view, fuzzy finding, multi-pane views, and bookmarking. Dive into the [User Guide](docs/USER_GUIDE.md) to master them all.

## 🛠️ Helpful Recipes

Watch silently for 30 seconds, save to a file, and exit (perfect for scripts):
```sh
./zyterm /dev/ttyUSB0 --dump 30 -l boot_capture.log
```

Highlight specific error lines so they grab your attention instantly:
```sh
./zyterm /dev/ttyUSB0 --watch ERROR --watch panic
```

Replay a captured log later with perfect timing:
```sh
./zyterm --replay boot_capture.log
```

## 📦 Installation

### Ubuntu / Debian (.deb)
You can download the latest `.deb` package from the [Releases page](https://github.com/iskandarputra/zyterm/releases) and install it in one simple step:
```sh
sudo dpkg -i releases/zyterm_*.deb
```

### Build from source
Wondering about dependencies? You only need a C compiler, `make`, and `pthread`.
```sh
./build.sh install
```
*(This helpful script automates formatting, linting, building, and installation for you).*

## 📚 Documentation

| Resource | What you will find |
| :--- | :--- |
| 📖 [User Guide](docs/USER_GUIDE.md) | Every flag, every shortcut, and advanced recipes. |
| 🧩 [API Docs](docs/API.md) | How to embed zyterm's core into your own C projects. |
| 🏗️ [Architecture](docs/ARCHITECTURE.md) | A deep dive into how zyterm manages memory, rendering, and concurrency without external libraries. |
| 🤝 [Contributing](docs/CONTRIBUTING.md) | Style guidelines, the strict "zero dependency" rule, and how to submit patches. |

## 🧠 What is Inside?

It is about **8.8k lines of plain C11**. zyterm is divided into ten modular components: core, serial, log, proto, render, tui, net, ext, and loop. 

We purposely avoided external dependencies, except for optional native X11 clipboard integration via `libxcb`. If you want to see what highly-optimized, POSIX-compliant C looks like, the `src/` directory is thoroughly documented and very approachable.

## ⚖️ License
MIT. See [LICENSE](LICENSE). Please feel free to use it however you like!

---
*zyterm stands on the shoulders of every serial-terminal tool that came before it. If you find it useful, we would love it if you considered starring the repo or contributing a patch!*
