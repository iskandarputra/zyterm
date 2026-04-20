# User Guide

A walkthrough of what zyterm can do and how to use it.

## What zyterm does

It opens a serial port, exchanges bytes with whatever is on the other
end, and shows you the result in a terminal UI. You get scrollback,
search, log capture, hex view, watch-pattern highlights, macros, and a
fuzzy command finder — the kinds of things that are handy when you're
talking to microcontrollers all day.

One binary, no external dependencies. The main loop is single-threaded;
there's an optional reader thread for the RX path if you're running at
very high baud rates and the main loop can't keep up.

```sh
./zyterm /dev/ttyUSB0 -b 115200          # the most common invocation
./zyterm --replay capture.log            # replay an old capture
./zyterm /dev/ttyUSB0 --dump 60 -l rx.txt   # headless 60-second capture
```

To quit at any time, press `Ctrl+A` then `q`.

## Command-line options

### Connection

| Flag                          | Default  | Meaning                                           |
| ----------------------------- | -------- | ------------------------------------------------- |
| `-b, --baud <rate>`           | 115200   | Any baud termios2 will accept (75 to 4 000 000+). |
| `--data <5\|6\|7\|8>`        | 8        | Data bits.                                        |
| `--parity <n\|e\|o>`         | n        | None, even, or odd.                               |
| `--stop <1\|2>`              | 1        | Stop bits.                                        |
| `--flow <n\|r\|x>`           | n        | None, RTS/CTS, or XON/XOFF.                      |
| `--reconnect`                 | on       | Auto-reopen device on hangup.                     |
| `--no-reconnect`              |          | Exit on hangup instead.                           |

### Logging and capture

| Flag                 | Meaning                                                            |
| -------------------- | ------------------------------------------------------------------ |
| `-l, --log <file>`   | Append log with millisecond timestamps. `Ctrl+A l` toggles inline. |
| `--log-max-kb <N>`   | Rotate to `<file>.1` when the log exceeds N kilobytes.             |
| `--tx-ts`            | Also log TX with `->` prefix and timestamps.                       |
| `--dump <sec>`       | Headless capture for N seconds. `0` means forever.                 |
| `--replay <file>`    | Replay a capture through the live UI.                              |
| `--replay-speed <x>` | Replay multiplier. `0` is as fast as possible.                     |

### Display and input

| Flag                 | Meaning                                                           |
| -------------------- | ----------------------------------------------------------------- |
| `-x, --hex`          | Render RX as a hex dump.                                          |
| `-e, --echo`         | Start with local echo on. Toggle inline with `Ctrl+A e`.          |
| `--no-color`         | Disable RX log-level colouring.                                   |
| `--ts`               | Start with timestamp display on. Toggle inline with `Ctrl+A t`.   |
| `--watch <pattern>`  | Highlight matching lines. Repeatable up to 8, each gets a colour. |
| `--watch-beep`       | Emit BEL on every watch match.                                    |
| `--macro F<n>=<str>` | Bind F1 to F12 to a string. Supports `\r`, `\n`, `\t`, `\xNN`.    |

### Misc

| Flag            | Meaning                 |
| --------------- | ----------------------- |
| `-h, --help`    | Show built-in help.     |
| `-V, --version` | Print version and exit. |

## Keys at runtime

| Key          | Action                                                                                                         |
| ------------ | -------------------------------------------------------------------------------------------------------------- |
| `Ctrl+A`     | Open the command menu popup (single-letter sub-commands below).                                                |
| F1 to F12    | Fire the bound macro (silent if unbound).                                                                      |
| PgUp / PgDn  | Scroll back or forward through scrollback.                                                                     |
| Up / Down    | Walk line-edit history.                                                                                        |
| Left / Right | Move the cursor.                                                                                               |
| Tab          | Remote completion (sync-and-flush). **Fast-path bypass** ensures instant render even during rapid completions. |
| `Ctrl+U`     | Clear the current line.                                                                                        |
| `Ctrl+W`     | Delete the previous word.                                                                                      |
| `Ctrl+L`     | Clear the screen.                                                                                              |
| `Ctrl+C`     | Send ETX (`0x03`) to the remote.                                                                               |

## Input and completion during log spam

When the serial device sends high-volume output (boot logs, debug traces,
sensor data), ordinary terminal tools become unusable: output scrolls past
faster than you can type, and completion tools stop responding.

zyterm solves this with ANSI scrolling regions (DECSTBM, an ANSI/VT100
standard escape sequence that defines which rows are allowed to scroll).
The screen is divided into three locked zones:

1. **Top bar**: Device name, baud rate, connection status. Always visible.
2. **Middle**: The actual log output. Scrolls freely as new data arrives.
3. **Bottom bar**: Your command line with cursor, prompt, and sent/unsent
   indicators. Always visible and always responsive.

During log spam, only the middle region scrolls. The input bar stays
locked at the bottom. You can:

- Type commands while output is streaming past
- Press Tab for shell completion (works with Zephyr RTOS and similar
  interactive shells on the device side)
- See your keystrokes rendered without noticeable lag
- Glance at the top bar for baud rate and error counters

This helps zyterm stay usable at high baud rates, though very fast
bursts on slow hardware may still occasionally drop characters — that's
usually a USB-serial adapter limitation rather than a software one.

## Mouse and selection

zyterm captures mouse input so you can select and copy text by
dragging. When you release, zyterm tries to put the text on your
system clipboard using a few approaches, in order:

1. **Native X11 clipboard** — On Linux desktops with X11 (or Wayland
   with XWayland), zyterm loads `libxcb.so.1` at runtime and becomes
   the CLIPBOARD selection owner. This means Ctrl+V paste works in
   other apps. No extra packages to install — `libxcb.so.1` is already
   present on most graphical systems.
2. **OSC 52 terminal escape** — Works over SSH, inside tmux, and on
   terminals that support it (most modern ones do).
3. **Helper binaries** — Falls back to `wl-copy` (Wayland), `xclip`,
   `xsel` (X11), or `pbcopy` (macOS), if any are available.
4. **File fallback** — As a last resort, saves to
   `~/.cache/zyterm/clipboard`. Not ideal, but at least the text
   isn't lost.

The selection persists when you scroll, so you can drag, scroll down,
and copy later. Right-click inside the selection to copy it again.
Press `Ctrl+A` then `m` to toggle mouse capture on/off (default is
**on**).

`Ctrl+A` then `Y` is a quick-copy shortcut: it copies the active
selection if there is one, otherwise the current output line, or
flashes the HUD if there's nothing to copy.

### Ctrl+A menu

| Key | Function                                                           |
| --- | ------------------------------------------------------------------ |
| `q` | Quit.                                                              |
| `x` | Send extended hex byte (e.g. `0x1B` for ESC).                      |
| `e` | Toggle local echo.                                                 |
| `c` | Toggle log-level colouring.                                        |
| `h` | Toggle hex view.                                                   |
| `t` | Toggle timestamp display.                                          |
| `l` | Toggle log capture to file.                                        |
| `b` | Send BREAK.                                                        |
| `r` | Force reconnect now.                                               |
| `/` | Search. Type your query, then `n` and `N` to step through matches. |
| `f` | Cycle flow control (none/RTS·CTS/XON·XOFF).                       |
| `a` | Toggle the auto-baud probe.                                        |
| `m` | Toggle mouse capture (on/off). Default is **on**.                  |
| `s` | Open the session picker (multi-window split or attach).            |
| `p` | Open the profile menu (saved baud, flow, macro presets).           |
| `o` | Open settings dialog (4-page serial/display/keyboard/logging).     |
| `k` | Show keybindings reference popup.                                  |
| `j` | Cycle log format (text/JSON/raw).                                  |
| `F` | Cycle framing mode (Raw/COBS/SLIP/HDLC/LenPfx).                   |
| `K` | Cycle CRC mode.                                                    |
| `G` | Toggle raw passthrough mode.                                       |
| `D` | Mute/unmute `<dbg>` log-level lines.                               |
| `I` | Mute/unmute `<inf>` log-level lines.                               |
| `Y` | Copy: selection, then log line, then flash. Keyboard clipboard.    |
| `.` | Open fuzzy finder over scrollback.                                 |
| `+` | Add a bookmark at current scrollback position.                     |
| `[` | Show bookmark list.                                                |

### Settings dialog (`Ctrl+A` then `o`)

A four-page settings menu with live changes. Use left/right to navigate
pages, press letter keys to cycle values, `Esc` to close.

**Page 1 — Serial Port**: Device, baud rate, data bits, stop bits,
parity, flow control, frame mode, CRC mode.

**Page 2 — Screen & Display**: Color mode, local echo, hex display,
timestamps, SGR passthrough, raw passthrough, mute `<dbg>` and `<inf>`.

**Page 3 — Keyboard & Misc**: Command key, mouse capture, watch beep,
OSC 52 clipboard, hyperlinks, pause, auto-reconnect.

**Page 4 — Logging**: Log status, log file path, log format
(text/JSON/raw), rotation limit, TX timestamps, bytes written.

## Recipes

### Capture a boot log to disk

```sh
./zyterm /dev/ttyUSB0 -b 921600 -l boot.log --tx-ts
```

This logs RX with millisecond timestamps and TX with a `->` prefix, so
you can later correlate stimulus with response.

### Headless capture for a CI run

```sh
./zyterm /dev/ttyACM0 --dump 30 -l ci-capture.log --no-color
```

No UI. It runs for 30 seconds and exits. Pair with
`--watch ERROR --watch-beep` if you want a build-server alert.

### Replay an old capture

```sh
./zyterm --replay boot.log              # real-time playback
./zyterm --replay boot.log --replay-speed 0   # as fast as possible
./zyterm --replay boot.log --replay-speed 4   # 4x speed
```

### Highlight RTOS log levels

```sh
./zyterm /dev/ttyUSB0 \
    --watch "ERROR" --watch "WARN" --watch "panic"
```

Each pattern gets its own colour. Add `--watch-beep` for an audible cue.

### Bind macros for repetitive prompts

```sh
./zyterm /dev/ttyUSB0 \
    --macro F1='version\r' \
    --macro F2='reboot\r' \
    --macro F3='\x1b[A'
```

F1, F2, and F3 now inject those bytes whenever pressed.

### Auto-baud probe

Power-cycle the target and press `Ctrl+A a`. zyterm cycles through
common rates (9600 up to 4 000 000) and locks on when it sees a clean
ASCII frame.

## Files and environment

| Name                  | Purpose                                                      |
| --------------------- | ------------------------------------------------------------ |
| `~/.zyterm/profiles`  | Saved baud, flow, and macro presets (managed by `Ctrl+A p`). |
| `~/.zyterm/bookmarks` | Per-device bookmark store (managed by `Ctrl+A b`).           |
| `~/.zyterm/history`   | Line-edit history.                                           |
| `$ZYTERM_TRACE`       | If set to a path, fatal paths append a trace record there.   |

## Troubleshooting

| Symptom                                 | Try this                                                        |
| --------------------------------------- | --------------------------------------------------------------- |
| `serial open ... Permission denied`     | Add yourself to the `dialout` group, then `newgrp dialout`.     |
| Garbage characters at high baud         | The cable or adapter is the bottleneck. Try `--flow r`.         |
| UI freezes after host suspend           | Press any key. zyterm restores the TTY on the first keypress.   |
| Embedder reports "second run is broken" | The host forgot `zt_embed_reset()`. See [API.md](API.md).       |
| Need a wire trace for a bug report      | Run `ZYTERM_TRACE=/tmp/zt.log ./zyterm ...` and attach the log. |

## Cheat sheet (printable)

```
                   z y t e r m
   ┌──────────────────────────────────────────┐
   │ Ctrl+A then ...                          │
   │   q quit         e echo       h hex      │
   │   t timestamp    c color      x hex byte │
   │   l log toggle   b bookmark   r reconnect│
   │   / search       f fuzzy      a autobaud │
   │   p profiles     s sessions   k keys     │
   │   o settings     D mute-dbg   I mute-inf │
   │ F1..F12  macros          PgUp/PgDn scroll│
   │ Tab remote-complete      Ctrl+L clear    │
   └──────────────────────────────────────────┘
```
