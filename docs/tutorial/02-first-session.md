# Chapter 2 — Your first session

Goal: open a serial device, see traffic, send a few bytes, and quit
cleanly.

## Step 1: find the port

Plug in your USB-serial adapter (FT232, CH340, CP210x, ST-Link, etc.)
and look at what appeared:

```sh
ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
# /dev/ttyUSB0
```

If nothing shows up:

```sh
dmesg | tail -30 | grep -i 'tty\|usb'
# usb 1-2: cp210x converter now attached to ttyUSB0
```

If you have several adapters and don't want to guess every time, use
the discovery flags instead of a fixed path (see chapter 7 for
recipes):

```sh
./zyterm --port-glob '/dev/ttyUSB*' -b 115200
./zyterm --match-vid-pid 1a86:7523 -b 115200      # CH340
```

## Step 2: connect

```sh
./zyterm /dev/ttyUSB0 -b 115200
```

If your board sends something at boot, you'll see it immediately. If
the line is idle (no power on the target, or it's mid-flash) the pane
just sits empty — that's normal.

The screen has three locked regions:

```
┌──────────────────────────────────────────────────────────────────┐
│ /dev/ttyUSB0  115200 8N1   ●●  RX 1.2k  TX 84 ↺0   raw  text    │   ← top status
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│ <inf> boot: starting zephyr v3.5                                 │
│ <dbg> sensors: imu ok                                            │   ← scrolling
│ <inf> shell: uart console ready                                  │     pane
│                                                                  │
├──────────────────────────────────────────────────────────────────┤
│ uart:~$ █                                                        │   ← input bar
└──────────────────────────────────────────────────────────────────┘
```

Even when the middle pane is scrolling at full tilt, the input bar
stays put — that's the whole point of the DECSTBM scrolling-region
trick (see [USER_GUIDE.md](../USER_GUIDE.md#input-and-completion-during-log-spam)).

## Step 3: type & send

By default zyterm transmits each line when you press **Enter**. Local
echo is **off** — you'll only see what you typed if the remote echoes
it back (typical for shells; not typical for raw streams).

| Key       | Action                                             |
| --------- | -------------------------------------------------- |
| Enter     | Send line + LF (or CR/CRLF, see `--map-out`).      |
| Backspace | Delete char left of cursor (line-buffered).        |
| Ctrl+U    | Clear the current line.                            |
| Ctrl+W    | Delete the previous word.                          |
| Up / Down | Walk line history.                                 |
| Tab       | Send Tab byte (handy for shells doing completion). |
| Ctrl+C    | Send `0x03` (ETX).                                 |
| Ctrl+L    | Clear the screen.                                  |

If you need raw character-by-character TX (no line buffering),
use `Ctrl+A G` to toggle raw passthrough.

## Step 4: try the command menu

Press **Ctrl+A** to pop the command menu. A small overlay appears.
Press a single letter:

| `Ctrl+A` then... | Does                                               |
| ---------------- | -------------------------------------------------- |
| `q`              | Quit.                                              |
| `?`              | Show every key binding (pageable).                 |
| `e`              | Toggle local echo.                                 |
| `h`              | Toggle hex view (RX rendered as a hex dump).       |
| `t`              | Toggle line timestamps in the pane.                |
| `l`              | Toggle log capture to `zyterm-YYYYMMDD-NNN.txt`.   |
| `b`              | Cycle through common baud rates without reconnect. |
| `r`              | Force reconnect.                                   |
| `/`              | Search scrollback (then `n`/`N` to step).          |
| `p`              | Open the fuzzy command palette.                    |
| `o`              | Open the 4-page settings dialog.                   |

A full menu reference is in [chapter 9](09-reference.md).

## Step 5: quit cleanly

```text
Ctrl+A q
```

zyterm restores your terminal (raw mode off, alt-screen released,
cursor visible) **before** running any other shutdown work — so even
if a USB device hangs on close, your shell prompt comes back clean.

## What just happened under the hood

1. `setup_serial()` opened `/dev/ttyUSB0` with `O_NOCTTY | O_NONBLOCK`,
   then configured 115200 8N1 via `termios2` (Linux-specific IOCTL
   that allows arbitrary baud rates, not just the POSIX list).
2. The main loop is `poll()`-based: it waits on stdin, the serial fd,
   and any of the optional sockets (HTTP, metrics, session). On data,
   it pushes RX into the renderer and TX into the device.
3. `render.c` repaints only the regions that changed. The top/bottom
   bars are repainted at most every ~33 ms; the scrolling pane uses
   the kernel's own scrolling-region escape so the OS terminal does
   the heavy lifting.
4. On quit, `restore_terminal()` runs first. Then scrollback,
   history, and watch buffers free, hooks reap any child processes
   (with a SIGALRM watchdog), and the program exits.

## Next

See [chapter 3 — Logging & capture](03-logging.md).
