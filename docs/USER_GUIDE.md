# User Guide

A walkthrough of what zyterm can do and how to use it.

> **Want the deep version?** See the multi-chapter
> [tutorial book](tutorial/00-index.md) — installation, profiles,
> hooks, recording, recipes, troubleshooting, and a full reference.

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

## First 5 minutes

A guided tour for someone who just cloned the repo.

**1. Build.** One dependency-free binary:

```sh
make -j
./zyterm --version
```

**2. Open your first port.** Plug in a USB-serial adapter and run:

```sh
./zyterm /dev/ttyUSB0 -b 115200
```

If you're not sure which device appeared, run
`ls /dev/ttyUSB* /dev/ttyACM*` or `dmesg | tail -20 | grep tty`.
Permission denied? Add yourself to the `dialout` (or
`uucp`) group and log out/in — don't `chmod 666` the device.

You're now connected. Type to transmit. Incoming bytes appear in the
pane. The status bar at the bottom shows RX/TX counters, CPS, and
current line-ending mode.

**3. Essential keys.** Every shortcut is `Ctrl+A` followed by a key:

| Keys       | Action                            |
| ---------- | --------------------------------- |
| `Ctrl+A q` | Quit.                             |
| `Ctrl+A ?` | Full key list (pager).            |
| `Ctrl+A l` | Start/stop logging to a file.     |
| `Ctrl+A /` | Search scrollback.                |
| `Ctrl+A x` | Toggle hex view.                  |
| `Ctrl+A e` | Toggle local echo.                |
| `Ctrl+A t` | Toggle timestamps on each line.   |
| `Ctrl+A b` | Change baud without reconnecting. |
| `Ctrl+A p` | Fuzzy command palette.            |

**4. Capture a session to disk.** Two flavours:

```sh
./zyterm /dev/ttyUSB0 -l boot.log                # timestamped text
./zyterm /dev/ttyUSB0 --rec boot.cast            # asciinema v2 cast
```

The `.cast` file replays in a browser (`asciinema play`) or on
asciinema.org. The `.log` file is plain text and `grep`-friendly.

**5. Reuse settings with profiles.** Drop a file in
`~/.config/zyterm/myboard.conf`:

```ini
device      = /dev/ttyUSB0
baud        = 115200
data_bits   = 8
parity      = n
stop_bits   = 1
reconnect   = true
map_out     = lf
```

Then just:

```sh
./zyterm --profile myboard
```

The profile is **hot-reloaded** — edit the file and zyterm picks up
runtime-safe keys (line-endings, log format, framing, OSC 52, etc.)
within ~200 ms with no reconnect needed. Connection-affecting keys
(`device`, `baud`, `data_bits`, `parity`, `stop_bits`) take effect on
the next reconnect (`Ctrl+A r`).

Profiles support **only the keys above** plus `osc52`, `frame`,
`crc`, `log_format`, `map_in`. For macros, watches, and event hooks,
write a small wrapper script around `zyterm --profile <name>`. See
[the deep tutorial](tutorial/04-profiles.md) for the full list and
the wrapper recipe.

**6. Automate responses.** The three `--on-*` flags fire shell actions
in reaction to session events:

```sh
./zyterm /dev/ttyUSB0 \
    --on-connect  'notify-send "board up"' \
    --on-disconnect 'notify-send "board down"' \
    --on-match    '/PANIC|BUG/=echo "$(date +%T) $ZYTERM_LINE" >> panics.log' \
    --on-match    '/login:/=send:root\n'
```

Prefix the action with `send:` to inject bytes back to the device.
Each hook is rate-limited to one fire per 100 ms.

That's it. Everything else in this guide is elaboration.

## Command-line options

### Connection

| Flag                    | Default | Meaning                                                                                  |
| ----------------------- | ------- | ---------------------------------------------------------------------------------------- |
| `-b, --baud <rate>`     | 115200  | Any baud termios2 will accept (75 to 4 000 000+).                                        |
| `--data <5\|6\|7\|8>`   | 8       | Data bits.                                                                               |
| `--parity <n\|e\|o>`    | n       | None, even, or odd.                                                                      |
| `--stop <1\|2>`         | 1       | Stop bits.                                                                               |
| `--flow <n\|r\|x>`      | n       | None, RTS/CTS, or XON/XOFF.                                                              |
| `--reconnect`           | on      | Auto-reopen device on hangup. Survives USB unplug.                                       |
| `--no-reconnect`        |         | Exit on hangup instead.                                                                  |
| `--port-glob <pat>`     |         | E.g. `/dev/ttyUSB*`. Re-resolves on every reconnect, so a USB-serial that comes back as a different node is transparently picked up. With this set, `<DEVICE>` is optional. |
| `--match-vid-pid <V:P>` |         | Hex USB IDs, e.g. `1a86:7523` (CH340). Combine with `--port-glob`.                       |
| `--autobaud`            | off     | Open at 115200, then probe a fixed list of common rates and pick the one with the highest printable-ASCII ratio. Overrides `-b`. Also fireable interactively with `Ctrl+A A`. |

### Logging and capture

| Flag                          | Meaning                                                                  |
| ----------------------------- | ------------------------------------------------------------------------ |
| `-l, --log <file>`            | Append log with millisecond timestamps. `Ctrl+A l` toggles inline.       |
| `--log-max-kb <N>`            | Rotate to `<file>.1` when the log exceeds N kilobytes.                   |
| `--log-format <text\|json\|raw>` | Log encoding (default `text`). `json` is structured per-line; `raw` is the unrewritten serial byte stream. |
| `--tx-ts`                     | Also log TX with `->` prefix and timestamps.                             |
| `--mute-dbg`                  | Drop `<dbg>` level lines from log + scrollback.                          |
| `--mute-inf`                  | Drop `<inf>` level lines from log + scrollback.                          |
| `--dump <sec>`                | Headless capture for N seconds. `0` means forever.                       |
| `--rec <file.cast>`           | Record the session as an asciinema v2 cast file.                         |
| `--replay <file>`             | Replay a capture through the live UI.                                    |
| `--replay-speed <x>`          | Replay multiplier. `0` is as fast as possible.                           |

### Framing and CRC

| Flag                              | Meaning                                                                  |
| --------------------------------- | ------------------------------------------------------------------------ |
| `--frame <raw\|cobs\|slip\|hdlc\|lenpfx>` | RX frame decoder. `lenpfx` is 16-bit little-endian length-prefixed.      |
| `--crc <none\|ccitt\|ibm\|crc32>` | Trailing CRC stripped and verified per frame. Mismatches surface as a flash. |

### Profiles and hooks

| Flag                       | Meaning                                                              |
| -------------------------- | -------------------------------------------------------------------- |
| `--profile <name>`         | Load `~/.config/zyterm/<name>.conf` and hot-reload on edits.         |
| `--profile-save <name>`    | Snapshot every CLI-supplied setting (+ `<DEVICE>` if given) to `~/.config/zyterm/<name>.conf` and exit. |
| `--on-connect <action>`    | Run shell action after each successful connect. Repeatable.          |
| `--on-disconnect <action>` | Run shell action on disconnect or exit. Repeatable.                  |
| `--on-match '/RE/=action'` | Run action on lines matching POSIX ERE. Prefix `send:` to inject TX. |

### Display and input

| Flag                          | Meaning                                                                  |
| ----------------------------- | ------------------------------------------------------------------------ |
| `-x, --hex`                   | Render RX as a hex dump.                                                 |
| `-e, --echo`                  | Start with local echo on. Toggle inline with `Ctrl+A e`.                 |
| `--no-color`                  | Disable RX log-level colouring.                                          |
| `--ts`                        | Start with timestamp display on. Toggle inline with `Ctrl+A t`.          |
| `--watch <pattern>`           | Highlight matching lines. Repeatable up to 8, each gets a colour.        |
| `--watch-beep`                | Emit BEL on every watch match.                                           |
| `--macro F<n>=<str>`          | Bind F1 to F12 to a string. Supports `\r`, `\n`, `\t`, `\xNN`.           |
| `--map-out <mode>` / `--map-in <mode>` | Rewrite outgoing / incoming line endings. Mode: `none \| cr \| lf \| crlf \| cr-crlf \| lf-crlf`. |
| `--osc52` / `--no-osc52`      | Push in-app selections to the system clipboard via OSC 52 (default on). |

### HTTP server and streaming

| Flag                | Meaning                                                                       |
| ------------------- | ----------------------------------------------------------------------------- |
| `--http <port>`     | Bring up the built-in HTTP server. Endpoints: `GET /` (HTML), `GET /stream` (SSE), `GET /ws` (WebSocket), `GET /api/state`, `POST /api/send`, `GET /metrics`. |
| `--webroot <dir>`   | Serve static files from `<dir>` (path traversal blocked).                     |
| `--http-cors`       | Add `Access-Control-Allow-Origin: *` headers.                                 |
| `--metrics <path>`  | AF\_UNIX socket emitting Prometheus-text counters.                            |

### Sessions and integration

| Flag                | Meaning                                                                       |
| ------------------- | ----------------------------------------------------------------------------- |
| `--detach <name>`   | Expose the live session on an AF\_UNIX socket so another zyterm can `--attach` from anywhere on the host. tmux-style. |
| `--attach <name>`   | Attach to a `--detach`ed session. Ctrl-\ detaches.                            |
| `--filter <cmd>`    | Pipe RX bytes through `sh -c <cmd>` before display. E.g. `--filter 'grep -v noise'`. |
| `--diff <a> <b>`    | Diff two capture files and exit.                                              |

### Performance / Advanced I/O

| Flag         | Meaning                                                                                                                            |
| ------------ | ---------------------------------------------------------------------------------------------------------------------------------- |
| `--threaded` | Drain serial on a dedicated worker thread into a 1 MiB SPSC ring. Reduces UART-latency jitter at ≥ 1 Mbaud. Pure overhead below ~500 kbaud — leave off by default. |

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

Keys are typed as `Ctrl+A` followed by the letter (e.g. `Ctrl+A q`).
Most letters are case-insensitive; uppercase variants that mean
something different are called out explicitly.

| Key       | Function                                                           |
| --------- | ------------------------------------------------------------------ |
| `q` / `x` | Quit.                                                              |
| `?` / `k` | Show the full keybindings reference popup. Any key dismisses it.   |
| `p`       | Pause / resume scrollback output (the log still writes if open).   |
| `e`       | Toggle local echo.                                                 |
| `c`       | Clear screen (also clears the in-flight line accumulator).         |
| `h`       | Toggle hex dump mode.                                              |
| `t`       | Toggle inline `[HH:MM:SS.mmm]` timestamps in scrollback.           |
| `l`       | Start / stop logging (auto-names `zyterm-YYYYMMDD-NNN.txt`).       |
| `n`       | Rename the current log file (only if a log is open).               |
| `b`       | Send BREAK.                                                        |
| `s`       | Print a one-shot RX/TX/lines/uptime stats line.                    |
| `r`       | Force reconnect now (closes and reopens the device).               |
| `/`       | Search scrollback. Type the query, then `n` / `N` step matches.    |
| `f`       | Cycle flow control (none / RTS·CTS / XON·XOFF).                    |
| `a`       | Send a literal `Ctrl+A` (0x01) byte to the device.                 |
| `A`       | Run the auto-baud probe at runtime.                                |
| `m`       | Toggle mouse capture (default ON). OFF lets the host terminal own selection. |
| `o`       | Open the settings dialog (4 pages).                                |
| `j`       | Cycle log format (text / JSON / raw).                              |
| `F`       | Cycle framing mode (raw / COBS / SLIP / HDLC / len16).             |
| `K`       | Cycle CRC mode (none / CCITT / IBM / CRC32).                       |
| `G`       | Enter / leave raw passthrough mode (KGDB / GDB stub).              |
| `D`       | Mute / unmute `<dbg>` level lines.                                 |
| `I`       | Mute / unmute `<inf>` level lines.                                 |
| `Y`       | Copy: current mouse selection if any, else the in-flight log line. |
| `.`       | Open the fuzzy finder over scrollback.                             |
| `+`       | Add a bookmark at the current scrollback position.                 |
| `[`       | Show the bookmark list.                                            |

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

## Surviving USB unplug

zyterm runs with `--reconnect` on by default, which means it treats
hot-unplug as a temporary state rather than a fatal error. When the
kernel reports the device gone (POLLHUP / `EIO`), zyterm closes the
fd, fires `--on-disconnect` hooks if any, and shows the centred
`⚠ connection interrupted` modal.

While the wait loop runs:

- The HUD displays a bright amber **`◆ DISCONNECTED`** pill, so the
  state stays visible even after you dismiss the modal.
- **Scrollback still works.** PgUp hides the modal automatically and
  lets you read back through everything that was on screen at the
  moment of the unplug. PgDn back to bottom brings the modal back.
- `Ctrl+A /` (search), `Ctrl+A Y` (copy), `Ctrl+A [` (bookmark list),
  `Ctrl+A o` (settings), `Ctrl+A k` / `?` (help) all work as normal.
- Side-effecting keys that need a live serial fd (`Ctrl+A a` / `A`,
  `Ctrl+A b`) are politely refused with a flash; nothing crashes.
- `Ctrl+A r` nudges the wait loop into an immediate retry.
- `Ctrl+A x` (or `q`) exits cleanly.

When the device reappears, zyterm reopens it, fires `--on-connect`
hooks, and shows a `✓ reconnected` flash. **The view stays where you
left it** — if you were reading scrollback, you stay in scrollback;
PgDn to bottom when you're ready to see live data again.

If you'd rather have zyterm exit on disconnect (the classic minicom /
screen behaviour), pass `--no-reconnect`.

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

Power-cycle the target and press `Ctrl+A A` (capital `A`). zyterm
cycles through common rates (9600 up to 4 000 000) and picks the one
that yields the highest printable-ASCII ratio. The CLI flag
`--autobaud` runs the same probe at startup before the first session.

(Note: lowercase `Ctrl+A a` sends a literal `0x01` byte to the device,
useful for `screen`-style apps on the other end that themselves use
`Ctrl+A` as a prefix.)

## Files and environment

| Name                              | Purpose                                                                                       |
| --------------------------------- | --------------------------------------------------------------------------------------------- |
| `~/.config/zyterm/<name>.conf`    | Profile files loaded with `--profile <name>` / written by `--profile-save <name>`. Honours `$XDG_CONFIG_HOME` if set. |
| `~/.cache/zyterm/clipboard`       | Last-resort clipboard fallback when no helper is available.                                   |
| `$ZYTERM_TRACE`                   | If set to a path, fatal paths append a trace record there.                                    |
| `$XDG_CONFIG_HOME`                | Overrides the `~/.config` profile directory.                                                  |
| `$NO_COLOR` / `$TERM=dumb`        | Forces monochrome `--help`.                                                                   |

Bookmarks (`Ctrl+A +` / `Ctrl+A [`) and line-edit history (Up/Down
arrows in the input bar) are in-memory only; they don't persist across
runs.

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
   │   q/x quit      ?/k help      o settings │
   │   p pause       e echo        c clear    │
   │   h hex         t timestamp   m mouse    │
   │   l log         n rename log  b BREAK    │
   │   r reconnect   f flow        s stats    │
   │   a ⇒0x01       A autobaud    Y yank     │
   │   / search      . fuzzy       + bookmark │
   │   [ bm list     j log fmt     F frame    │
   │   K crc         G passthrough             │
   │   D mute-dbg    I mute-inf               │
   │                                          │
   │ F1..F12  macros        PgUp/PgDn scroll  │
   │ Tab remote-complete    Ctrl+L clear      │
   └──────────────────────────────────────────┘
```
