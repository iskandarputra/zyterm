# zyterm user guide

zyterm is a single-binary Linux serial-terminal emulator. These pages are
task-oriented: each one walks you through doing something real, then points at
the reference catalogues when you need the exhaustive list. For "why is it built
this way" see the [decisions](../decisions/README.md); for "how it works inside"
see the [reference](../reference/ARCHITECTURE.md).

This guide tracks zyterm **1.3.0**. Every keystroke and flag below is verified
against the source; if something here disagrees with the binary, the binary wins
and the guide is the bug.

## 60-second first session

```sh
make                              # builds ./zyterm (release, -O3)
./zyterm /dev/ttyUSB0 -b 115200   # connect at 115200 baud
```

You're now attached. Type a line and press `Enter` to send it. Then:

- `Ctrl+A` enters command mode — the menu pops up.
- `Ctrl+A ?` opens the full keybinding reference (any key closes it).
- `Ctrl+A q` quits and restores your shell.

That's the whole loop: connect, type, send, read, quit. Everything else —
logging, framing, the HTTP bridge, profiles — layers on top of this.

If you don't know the device node, plug the adapter in and run `ls /dev/ttyUSB*`
or `ls /dev/ttyACM*`. zyterm can also find it for you with `--port-glob` or
`--match-vid-pid` (see [getting-started](getting-started.md)).

## The pages

| Page | What you'll learn |
| --- | --- |
| [getting-started.md](getting-started.md) | Install/build, first connection, the HUD, typing & sending, local echo, scrollback + search, the command menu, quitting. |
| [logging-and-capture.md](logging-and-capture.md) | `--log` and the text/json/raw formats, rotation, timestamps, headless `--dump`, `--replay`, and asciinema `--rec`. |
| [automation.md](automation.md) | Profiles, event hooks, RX filters, detach/attach sessions, the HTTP/SSE/WebSocket bridge, and Prometheus metrics. |
| [recipes.md](recipes.md) | Copy-paste invocations for common board-bring-up, capture, and integration jobs. |
| [troubleshooting.md](troubleshooting.md) | Garbled output, disconnects, clipboard quirks, and features that look present but aren't wired up yet. |

## Reference catalogues

When you want the complete, unabridged list rather than a guided path:

- [reference/CLI.md](../reference/CLI.md) — every command-line flag, its argument
  grammar, default, and validation.
- [reference/KEYBINDINGS.md](../reference/KEYBINDINGS.md) — every interactive key,
  including command mode, input editing, scrollback, and mouse.
- [reference/FAQ.md](../reference/FAQ.md) — short answers, including a few "that
  used to be documented but isn't real" corrections.

## A note on honesty

A handful of things you might see in menus or older write-ups do not actually
work in 1.3.0 — the OSC 8 hyperlink toggle and an unwired epoll/splice fast path
among them. This guide never tells you to
use them as if they worked; where it matters, the relevant page links to
[tracking/KNOWN_ISSUES.md](../tracking/KNOWN_ISSUES.md) or the
[roadmap](../plans/ROADMAP.md) instead.
