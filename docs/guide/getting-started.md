# Getting started

This page takes you from an unbuilt source tree to a comfortable interactive
session: building, connecting, reading the heads-up display, typing and sending,
local echo, scrolling back and searching, the command menu, and quitting
cleanly. By the end you'll know the dozen keystrokes that cover day-to-day use.

For the exhaustive flag list see [reference/CLI.md](../reference/CLI.md); for
every key see [reference/KEYBINDINGS.md](../reference/KEYBINDINGS.md).

## Install / build

zyterm is plain C11 with a hand-written Makefile. The only build requirement is
a C compiler and `make`; the only runtime dependency is your system's libc.
Linux is the supported, CI-tested target.

```sh
git clone https://github.com/iskandarputra/zyterm.git
cd zyterm
make                 # release build (-O3), produces ./zyterm
```

To install it on your `PATH`:

```sh
sudo make install
```

There are a few other targets if you're hacking on it: `make debug` (`-O0 -g3`),
`make test` (unit + pty + integration), and `make modules` (prints the module
breakdown). You normally never need them just to use zyterm.

If your user isn't in the `dialout` group, opening `/dev/ttyUSB0` may fail with
a permission error — add yourself (`sudo usermod -aG dialout $USER`) and log back
in, or run zyterm under `sudo` for a one-off.

## Your first connection

Point zyterm at a device node and pick a baud rate:

```sh
./zyterm /dev/ttyUSB0 -b 115200
```

`-b/--baud` defaults to 115200, so for the common case you can drop it. Baud is
parsed as an arbitrary integer (1 to 20,000,000) and applied via `termios2`, so
non-standard rates work too (`src/main.c:280`). Frame format defaults to 8 data
bits, no parity, 1 stop bit, no flow control; override with `--data`, `--parity`,
`--stop`, and `--flow` if your device needs something else.

Don't know the node? Let zyterm discover it:

```sh
./zyterm --port-glob '/dev/ttyUSB*'              # first match wins
./zyterm --match-vid-pid 1a86:7523               # CH340 by USB VID:PID (hex)
```

With either discovery flag the positional `<DEVICE>` becomes optional
(`src/main.c:705`). You can also connect over the network instead of a local tty:
`tcp://host:port` for raw TCP (e.g. ser2net in raw mode) or `telnet://host:port`
for TCP with Telnet IAC handling.

When the link opens, zyterm switches to the alternate screen and prints a short
banner naming the device and baud, then drops you into the live session.

## Reading the HUD

The screen is split into three zones:

- A **status line** at the top showing the device, baud, byte counters, and the
  state of toggles like logging, hex mode, and timestamps.
- A large **scrollback body** in the middle where received bytes land.
- An **input bar** at the bottom — the line you're composing before you send it.

The HUD repaints at roughly 60 fps and is rate-capped so a board spewing boot
logs at high baud doesn't make the input bar stutter (`src/loop/runtime.c:241`).
Incoming bytes still hit scrollback and the log immediately; only the redraw is
coalesced.

zyterm captures the mouse by default so the wheel scrolls *its* scrollback (not
the host terminal's empty alt-screen) and click-drag selects text. To use your
host terminal's native selection instead, hold `Shift` while dragging, or toggle
capture off with `Ctrl+A m`.

## Typing and sending

Type a line; it accumulates in the input bar. Press `Enter` to send it — zyterm
appends a carriage return (`\r`) and transmits (`src/loop/input.c:948`). Line
editing works the way you'd expect:

| Key | Action |
| --- | --- |
| `Backspace` | delete the character before the cursor |
| `Ctrl+U` | clear the whole input line |
| `Ctrl+W` | delete the word before the cursor |
| `Left` / `Right` | move the cursor within the line |
| `Home` / `End` | jump to start / end of the line |
| `Up` / `Down` | walk back and forward through line history |
| `Tab` | send a tab and capture the remote's completion echo |
| `Ctrl+C` | send ETX (`0x03`) to the remote |
| `Ctrl+L` | redraw / clear the screen |
| `Esc` | discard the current input line |

History is kept for the session so `Up` recalls what you sent earlier. It lives
in memory only — see "What isn't saved" below.

If you need to send line endings other than a bare `\r`, rewrite them on the way
out with `--map-out` (modes: `none`, `cr`, `lf`, `crlf`, `cr-crlf`, `lf-crlf`),
and normalise what arrives with `--map-in`.

## Local echo

By default zyterm does **not** echo your keystrokes locally — it assumes the
remote echoes them back, which is the normal case for a shell or U-Boot prompt.
If the far end is silent (a raw sensor, a one-way link), turn on local echo so
you can see what you're typing:

- start with it on: `./zyterm /dev/ttyUSB0 -e`
- toggle it live: `Ctrl+A e` (`src/loop/input.c:84`)

A flash in the HUD confirms the new state. If you suddenly see every character
doubled, the remote *is* echoing and you should turn local echo back off.

## Scrollback and search

Received output is retained in a scrollback ring. Page through it with:

- `PgUp` / `PgDn` — scroll a screen at a time.
- `Shift+PgUp` / `Shift+PgDn` — same, for terminals that map the shifted codes.
- mouse wheel — scroll a few lines at a time (when mouse capture is on).
- `Up` / `Down` while already scrolled — move one line at a time.

Typing anything that isn't a scroll key leaves scroll mode and snaps back to the
live tail.

To find something, press `Ctrl+A /`, type a query, and press `Enter`. zyterm
jumps to the most recent match and flashes the hit number. While you're scrolled
into a result, `n` and `N` step to the next and previous match
(`src/loop/input.c:915`). Search runs over the scrollback you have; it isn't a
grep over a log file (for that, log to disk — see
[logging-and-capture.md](logging-and-capture.md)).

You can copy what you find: drag to select, and on release the selection is
pushed to your system clipboard via OSC 52 (and the native X11 selection where
available). `Ctrl+A Y` yanks the active selection — or, if there's none, the line
currently being assembled — which is handy when your terminal silently drops the
OSC 52 push. Right-clicking an existing selection re-copies it.

## The command menu

`Ctrl+A` is the prefix for every interactive command. Press it and a menu pops
up; press a second key to act. The keys you'll reach for most:

| Keys | Action |
| --- | --- |
| `Ctrl+A ?` (or `Ctrl+A k`) | full keybinding reference popup |
| `Ctrl+A q` / `Ctrl+A x` | quit |
| `Ctrl+A e` | toggle local echo |
| `Ctrl+A h` | toggle hex-dump rendering of RX |
| `Ctrl+A t` | toggle timestamp display |
| `Ctrl+A c` | clear the screen |
| `Ctrl+A p` | pause stdout (the log keeps writing) |
| `Ctrl+A s` | print a stats line (RX/TX bytes, uptime, throughput) |
| `Ctrl+A l` | start/stop logging to an auto-named file |
| `Ctrl+A r` | reconnect now |
| `Ctrl+A b` | send a serial BREAK |
| `Ctrl+A o` | open the settings menu (4 pages: serial / screen / keyboard / logging) |

The settings menu (`Ctrl+A o`) is a paged dialog: `Left`/`Right` or `Tab` move
between pages, the per-row letter toggles that setting, and `Esc` or `q` closes
it (`src/loop/input.c:475`).

Two command keys are easy to misread, so to be explicit: `Ctrl+A a` sends a
literal `0x01` (Ctrl+A) byte *to the device*, and `Ctrl+A A` (capital) runs an
autobaud probe. Older notes claimed `a` was autobaud — it isn't.

The `Ctrl+A .` "fuzzy history" finder filters your input history as you type.
It was non-functional before 1.3.0 — it scanned from an always-NULL slot and no
keystrokes reached it — and was repaired in the ZT-008 fix; see
[tracking/KNOWN_ISSUES.md](../tracking/KNOWN_ISSUES.md).

## Quitting

Press `Ctrl+A` then `q`, `Q`, `x`, or `X` (`src/loop/input.c:75`). zyterm flushes
output, restores your terminal out of raw mode and the alternate screen, and
returns control on a fresh line. If a final close happens to hang (e.g. a yanked
USB adapter), a 3-second watchdog (`src/main.c:789`) guarantees the process still
exits with your terminal already restored — you won't be left in a stuck raw-mode
shell.

## What isn't saved

Logs are real files on disk — see [logging-and-capture.md](logging-and-capture.md).
But your **command history and bookmarks live in memory only** and are gone when
you quit. There is no `~/.zyterm_history` or bookmarks file; the rationale is
recorded in [decisions/0006-in-memory-history-and-bookmarks.md](../decisions/0006-in-memory-history-and-bookmarks.md).
Connection settings, by contrast, *can* be persisted — save them with
`--profile-save <name>` (written to `~/.config/zyterm/<name>.conf`) and reload
with `--profile <name>`, covered in [automation.md](automation.md).
