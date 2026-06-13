# Keybindings — interactive reference

This is the authoritative catalogue of every key zyterm reads while running
interactively. It is derived directly from `src/loop/input.c`; where a binding
is subtle or easy to misremember, the source line is cited inline. If a key is
not listed here, zyterm does not bind it.

Two layers matter:

- **Normal input** — what you type goes to the device (TX line editor).
- **Command mode** — press `Ctrl+A` first, then a single command key.

Related: [CLI flags](CLI.md) set the startup defaults that these keys toggle ·
[FAQ](FAQ.md) for behavior questions · the in-app help popup (`Ctrl+A k`) is a
short version of these tables.

---

## Command mode (`Ctrl+A` then a key)

`Ctrl+A` (byte `0x01`) enters command mode and, unless a disconnect dialog is
showing, draws a command popup (`input.c:932`). The **next** key runs one
command and returns you to normal input. Command keys are case-folded except
where a lowercase and uppercase letter mean different things — those rows are
called out explicitly.

| Key | Action | Notes |
|-----|--------|-------|
| `q` `Q` `x` `X` | Quit zyterm | All four set the quit flag (`input.c:75-78`). **`x` quits — it does not send a hex byte.** |
| `p` `P` | Pause/resume stdout | Logging keeps writing while paused (`input.c:79`). |
| `e` `E` | Toggle local echo | `input.c:84`. |
| `h` `H` | Toggle hex render | Flushes the pending line/row on switch (`input.c:89`). |
| `t` `T` | Toggle timestamps | Repaints scrollback (`input.c:106`). |
| `c` `C` | **Clear the screen** | Resets the line/hex state and re-lays-out (`input.c:112`). **`c` clears — it is not a color toggle.** |
| `b` `B` | Send a serial BREAK | `tcsendbreak` (`input.c:118`). Refused while disconnected. |
| `a` | **Send literal `0x01` to the device** | `direct_send(0x01)` (`input.c:125`). This is how you transmit a Ctrl+A byte. **`a` is not autobaud.** |
| `A` | Run autobaud probe | Closes the fd, probes, reopens (`input.c:130`). Needs a device. |
| `s` `S` | Print stats line | RX/TX/lines/uptime/avg (`input.c:142`). |
| `l` `L` | Toggle logging | When enabling, auto-names `zyterm-YYYYMMDD-NNN.txt` in the current directory (`input.c:153`). |
| `f` | Cycle flow control | none → RTS/CTS → XON/XOFF (`input.c:192`). Lowercase only. |
| `r` `R` | Manual reconnect | Closes and re-enters the reconnect loop (`input.c:202`). |
| `m` `M` | Toggle mouse capture | See [Mouse](#mouse) (`input.c:224`). |
| `n` `N` | Rename the active log | Only when a log is open (`input.c:247`). |
| `/` | Search scrollback | Opens the search bar (`input.c:260`); then use `n`/`N`. |
| `k` `?` | Keybind help popup | Stays open until any key (`input.c:268`). |
| `j` `J` | Cycle log format | text → json → raw (`input.c:270`). |
| `F` | Cycle framing mode | Uppercase only (lowercase `f` is flow); `raw/cobs/slip/hdlc/lenpfx` (`input.c:278`). |
| `K` | Cycle CRC mode | none/ccitt/ibm/crc32 (`input.c:284`). |
| `G` | Toggle KGDB/raw passthrough | `input.c:288`. |
| `+` | Add a bookmark | At the current scrollback offset (`input.c:294`). Lowercase `b` is BREAK. |
| `[` | Bookmark list | `input.c:297`. |
| `Y` | Yank to clipboard | Selection if active, else the current line (`input.c:298`). |
| `.` | Fuzzy finder | Filter command history; `Enter` recalls, `Esc` cancels (`input.c`) — see below. |
| `D` | Mute `<dbg>` lines | `input.c:317`. |
| `I` | Mute `<inf>` lines | `input.c:321`. |
| `o` `O` | Open the settings menu | 4 pages (`input.c:326`); see [Settings menu](#settings-menu-ctrla-o). |

> **Corrections to the old tutorial.** Earlier docs listed `a`=autobaud,
> `c`=color, and `x`=send-hex-byte. **All three are wrong.** The verified
> meanings are `a` = send literal `0x01`, `c` = clear screen, `x` = quit
> (`input.c:75-128`). Use them as documented here.

### While disconnected

During a disconnect/reconnect dialog, command mode still works but keys that
need a serial fd are refused with a flash message: `a`, `A`, `b`, `B`
(`input.c:50-58`). `r`/`R` nudges the wait loop; everything else (scroll,
search, copy, settings, log toggle, help) stays available so you can review and
copy what was on screen before the unplug.

### Fuzzy finder (ZT-008, fixed)

`Ctrl+A .` opens a fuzzy finder over your command history. Type to filter (a cheap
subsequence scorer ranks matches), `Enter` recalls the selected entry into the input
line, `Esc` cancels. Fixed in the 2026-06 hardening — the match loop now scans from
history index 1 and `handle_stdin_chunk` routes keystrokes to it (with the off-by-one
**ZT-023** also closed); see [KNOWN_ISSUES](../tracking/KNOWN_ISSUES.md). `Up`/`Down`
still work for sequential history recall.

---

## Settings menu (`Ctrl+A o`)

`Ctrl+A o` opens a 4-page settings overlay (`input.c:326`, pages defined at
`input.c:339`). Navigation is shared across pages:

| Key | Action |
|-----|--------|
| `Left` / `Right` arrow | Previous / next page (`input.c:483`) |
| `Tab` | Next page (`input.c:496`) |
| `Esc` / `q` / `Q` | Close the menu (`input.c:477`) |

Each page binds letter keys to toggles. Keys are case-insensitive.

### Page 1 — Serial (`settings_handle_serial`, `input.c:358`)

| Key | Action |
|-----|--------|
| `b` | Cycle baud (9600 … 2000000) |
| `c` | Cycle data bits (8→7→6→5→8) |
| `d` | Cycle parity (n→e→o→n) |
| `e` | Toggle stop bits (1↔2) |
| `f` | Cycle flow control (none/RTS-CTS/XON-XOFF) |
| `g` | Cycle framing mode |
| `h` | Cycle CRC mode |

### Page 2 — Screen (`settings_handle_screen`, `input.c:402`)

| Key | Action |
|-----|--------|
| `a` | Toggle color |
| `b` | Toggle local echo |
| `c` | Toggle hex render |
| `d` | Toggle timestamps |
| `e` | Toggle SGR passthrough |
| `f` | Toggle KGDB/raw passthrough |
| `g` | Mute `<dbg>` |
| `h` | Mute `<inf>` |

### Page 3 — Keyboard (`settings_handle_kbd`, `input.c:442`)

| Key | Action |
|-----|--------|
| `b` | Toggle mouse capture |
| `c` | Toggle watch-beep |
| `d` | Toggle OSC 52 clipboard |
| `e` | Toggle **OSC 8 hyperlinks** — **no-op today** (see below) |
| `f` | Toggle pause |
| `g` | Toggle reconnect |

### Page 4 — Logging (`settings_handle_logging`, `input.c:465`)

| Key | Action |
|-----|--------|
| `c` | Cycle log format (text/json/raw) |
| `e` | Toggle TX timestamps |

> **The "OSC 8 hyperlinks" toggle does nothing (ZT-019).** Page 3 key `e`
> flips `c->proto.hyperlinks` (`input.c:456`), but the only consumer of that
> flag is the HUD label (`src/tui/hud.c:643`). The rewrite routine
> `osc8_rewrite()` (`src/proto/osc.c:238`) has **zero call sites**, so no RX
> bytes are ever turned into hyperlinks regardless of the toggle. Tracked as
> **ZT-019** in [KNOWN_ISSUES](../tracking/KNOWN_ISSUES.md).

---

## Normal input — editing keys

These act on the TX line you are composing. Printable bytes are inserted at the
cursor; with local echo on they are also sent immediately.

| Key | Action | Source |
|-----|--------|--------|
| `Enter` (`\r`/`\n`) | Push to history, flush unsent, send `\r` | `input.c:948` |
| `Tab` | Send `\t`, capture the completion echo back into the buffer | `input.c:960` |
| `Backspace` (`0x7F`/`0x08`) | Delete the char before the cursor | `input.c:972` |
| `Ctrl+U` (`0x15`) | Clear the whole line (sends `0x7F` for already-sent bytes) | `input.c:977` |
| `Ctrl+W` (`0x17`) | Delete the word before the cursor | `input.c:993` |
| `Ctrl+L` (`0x0C`) | Redraw the screen (re-layout) | `input.c:1003` |
| `Ctrl+C` (`0x03`) | Send ETX (`0x03`) to the device, clear the line | `input.c:1007` |
| `Esc` (`0x1B`, alone) | Clear the current input line | `input.c:941` |
| `Ctrl+A` (`0x01`) | Enter command mode | `input.c:932` |

---

## Arrows, history, and cursor

These work only at the start of a line (`sent_len == 0`) and are read as escape
sequences (`input.c:723`).

| Key | Action | Source |
|-----|--------|--------|
| `Up` | Previous history entry | `input.c:725` |
| `Down` | Next history entry (empty at the bottom) | `input.c:732` |
| `Left` | Cursor left | `input.c:748` |
| `Right` | Cursor right | `input.c:744` |
| `Home` | Cursor to start of line | `input.c:752` |
| `End` | Cursor to end of line | `input.c:756` |

> History is **in-memory only** — it is not written to disk and is lost on exit.
> See [FAQ](FAQ.md) and [ADR-0006](../decisions/0006-in-memory-history-and-bookmarks.md).

---

## Scrollback and the pager

Scrollback intercepts page keys regardless of input state (`input.c:547`).

| Key | Action | Source |
|-----|--------|--------|
| `PgUp` | Scroll up one page | `input.c:551` |
| `PgDn` | Scroll down one page | `input.c:555` |
| `Shift+PgUp` / `Shift+PgDn` | Same as above (alternate escape form) | `input.c:561` |
| `Up` / `Down` (while scrolled) | Scroll one line | `input.c:710` |
| Any other key (while scrolled) | Leave scroll mode | `input.c:720` |

### Search and the `n`/`N` pager

`Ctrl+A /` opens the search bar; type a query and press `Enter` to jump to the
first match (`input.c:879`). Afterwards, while still scrolled with an active
query:

| Key | Action | Source |
|-----|--------|--------|
| `n` | Next match | `input.c:915` |
| `N` | Previous match | `input.c:915` |

If you are not in scroll mode with a query, `n`/`N` are ordinary input bytes.

---

## F1–F12 macros

Each function key fires its configured macro (`input.c:527`,
`fkey_index_consume` in `src/proto/macros.c:37`). Macros are set with
`--macro F<n>=<str>` (n 1–12; `\r \n \t \xNN` escapes — see [CLI](CLI.md)). An
empty slot is a no-op. The decoder consumes the F-key prefix and, if the
terminal coalesced more bytes after it, recurses on the remainder so the next
keystroke is not lost.

| Keys | Encoding recognized |
|------|---------------------|
| `F1`–`F4` | SS3 (`ESC O P..S`) |
| `F5`–`F12` | CSI (`ESC [ <n> ~`) |

---

## Mouse

Mouse handling depends on the capture toggle (`Ctrl+A m`, `input.c:224`).

**Capture ON (default):** zyterm consumes mouse events to drive in-app
selection and scrolling.

| Action | Result | Source |
|--------|--------|--------|
| Wheel up / down | Scroll scrollback by 3 lines | `input.c:600` |
| Left drag in the body | Text selection → clipboard (OSC 52 + native X11) | `input.c:635` |
| Left press on the right-edge scrollbar + drag | Jump/drag scroll | `input.c:623` |
| Right-click on an existing selection | Re-copy the selection | `input.c:682` |

To make a native host-terminal selection while capture is on, hold **Shift**
to bypass tracking.

**Capture OFF:** the host terminal owns the mouse (native shift-free
selection), but in-app wheel scrolling and the scrollbar are inert
(`input.c:242`).

---

_See also: [CLI](CLI.md) · [FAQ](FAQ.md) ·
[KNOWN_ISSUES](../tracking/KNOWN_ISSUES.md) ·
[ADR-0006](../decisions/0006-in-memory-history-and-bookmarks.md)._
