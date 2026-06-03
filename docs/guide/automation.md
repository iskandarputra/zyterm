# Automation: profiles, macros, and hooks

This guide covers the three ways to make zyterm do work for you without typing the same
things over and over: **profiles** (saved configurations you can reload and hot-edit),
**F-key macros** (canned byte sequences), and **event hooks** (run a command or inject
bytes when something happens on the wire).

All three are configured from the command line. None of them require a config file to
exist up front — profiles create one for you with `--profile-save`.

See also: [CLI reference](../reference/CLI.md) · [Keybindings](../reference/KEYBINDINGS.md) ·
[Recipes](recipes.md) · [Troubleshooting](troubleshooting.md).

---

## Profiles

A profile is a tiny INI file holding the connection and protocol settings you reach for
on a given board. You load one with `--profile <name>` and snapshot the current flags
into one with `--profile-save <name>`.

### Where profiles live

Profiles are stored at:

```
$XDG_CONFIG_HOME/zyterm/<name>.conf      # if XDG_CONFIG_HOME is set
~/.config/zyterm/<name>.conf             # otherwise
```

(`src/ext/profile.c:39` resolves the path; parent directories are created with mode
`0700` on save.) There is no separate "profiles directory" registry — a profile is just
its `.conf` file, named after the profile.

### Saving a profile

Pass `--profile-save <name>` alongside the flags you want to capture. zyterm snapshots
**every CLI-supplied setting** (the save happens after the whole argument list is parsed,
so flags written after `--profile-save` are captured too) and then exits before opening
the device:

```sh
zyterm --baud 921600 --parity n --frame slip --crc ccitt \
       --map-out crlf --profile-save myboard
```

That writes `~/.config/zyterm/myboard.conf`:

```ini
# zyterm profile: myboard
device = /dev/ttyUSB0
baud = 921600
data_bits = 8
parity = n
stop_bits = 1
reconnect = true
osc52 = true
frame = slip
crc = ccitt
log_format = text
map_out = crlf
map_in = none
```

### Recognized keys

The loader (`src/ext/profile.c:78`) understands exactly these keys; anything else in the
file is ignored on load (and dropped on the next save — the round-trip is not loss-less
for unknown keys):

| Key          | Values                                  | Maps to            |
|--------------|-----------------------------------------|--------------------|
| `device`     | a path or URL                           | the connect target |
| `baud`       | integer                                 | `--baud`           |
| `data_bits`  | `5`–`8`                                  | `--data`           |
| `parity`     | `n` / `e` / `o`                          | `--parity`         |
| `stop_bits`  | `1` / `2`                                | `--stop`           |
| `reconnect`  | `true` / `false`                         | `--reconnect`      |
| `osc52`      | `true` / `false`                         | `--osc52`          |
| `frame`      | `raw`/`cobs`/`slip`/`hdlc`/`lenpfx`      | `--frame`          |
| `crc`        | `none`/`ccitt`/`ibm`/`crc32`             | `--crc`            |
| `log_format` | `text`/`json`/`raw`                      | `--log-format`     |
| `map_out`    | `none`/`cr`/`lf`/`crlf`/`cr-crlf`/`lf-crlf` | `--map-out`     |
| `map_in`     | same as `map_out`                       | `--map-in`         |

Lines beginning with `#` or `;` are comments.

### Loading a profile

```sh
zyterm --profile myboard /dev/ttyUSB0
```

Flags on the command line and the profile compose by order: `--profile` applies the
file's values at the point it appears, so a later explicit flag overrides the profile,
and an earlier one is overridden by it. A positional device on the command line always
wins over the profile's `device` key if it comes after `--profile`.

### Hot-reload (inotify)

When you start with `--profile <name>`, zyterm watches the profile file and reloads it
automatically when you save an edit — no restart needed
(`src/ext/profile_watch.c`). You'll see a `config reloaded:` notice in the scrollback.

A few details worth knowing:

- The watch is on the *parent directory*, filtered by filename, so it survives the
  atomic-rename save that vim, neovim, helix, VS Code, `sed -i`, and friends use
  (`src/ext/profile_watch.c:9`).
- Bursts of events from a single save are coalesced within a 200 ms debounce window, so
  one `:w` triggers one reload (`src/ext/profile_watch.c:56`).
- **Runtime-safe keys take effect immediately** (line-ending maps, log format, framing
  enum, CRC enum, reconnect/osc52 toggles). **`baud`, `device`, `parity`, and the other
  serial-line settings are loaded into memory but do not reach the live serial fd until
  you reconnect** (`Ctrl+A r`, or a hang-up if `--reconnect` is on). zyterm prints a
  notice reminding you of this (`src/ext/profile_watch.c:146`).

> **Note:** editing a `device =` key in a watched profile while connected is currently
> unsafe — the reload path can free a non-heap device string. See
> [ZT-001](../tracking/issues/ZT-001-profile-load-frees-argv-device.md)
> ([INVARIANTS §1](../invariants/INVARIANTS.md)). Prefer reconnecting manually after a
> device change rather than relying on hot-reload for that key.

---

## F-key macros

A macro binds one of the function keys **F1–F12** to a byte string that is sent to the
device when you press the key. Define them with `--macro F<n>=<string>`:

```sh
zyterm --macro F1='version\r' \
       --macro F2='reboot\r' \
       --macro F5='\x03' \
       /dev/ttyUSB0
```

`n` must be `1`–`12` (`src/main.c:533`). Pressing `F1` then sends `version\r` over the
wire. Macros are sent with the same trickle/flow-aware path as typed input
(`macro_fire`, `src/proto/macros.c:126`).

### Escape sequences

The string is expanded by `expand_escapes` (`src/proto/macros.c:87`). Supported escapes:

| Escape | Meaning                          |
|--------|----------------------------------|
| `\r`   | carriage return (0x0D)           |
| `\n`   | line feed (0x0A)                 |
| `\t`   | tab (0x09)                       |
| `\\`   | a literal backslash              |
| `\xNN` | one byte from **exactly two** hex digits (e.g. `\x1b` = ESC) |

Any other `\X` emits the literal character `X`. Note that `\xNN` consumes precisely two
hex digits; there is no octal `\NNN` escape. To send a control byte, use its hex form —
e.g. `\x03` for Ctrl-C/ETX, `\x04` for EOT, `\x1a` for the XMODEM/CP-M EOF.

The expanded macro is capped at 1 KiB (`exp[1024]`).

> Tip: `--macro` accepts the key with a lowercase `f` too (`F1` or `f1`). The argument
> form is strict: it must contain `=`, e.g. `--macro F3=AT\r`.

---

## Event hooks

Hooks let zyterm react to the device. There are three event kinds, all registered from
the command line and implemented in `src/ext/hooks.c`:

| Flag                          | Fires when…                                         |
|-------------------------------|-----------------------------------------------------|
| `--on-match '/REGEX/=ACTION'` | a received line matches the POSIX extended regex     |
| `--on-connect '<ACTION>'`     | the device opens                                     |
| `--on-disconnect '<ACTION>'`  | the device closes / hangs up                         |

Up to 16 hooks may be registered total (`ZT_HOOK_MAX`, `src/ext/hooks.c:51`). Each hook
is rate-limited to fire at most once per 100 ms, so a device spewing the same line cannot
fork thousands of children (`src/ext/hooks.c:52`).

### The `--on-match` spec

The argument is `/PATTERN/=ACTION`. The pattern is everything between the leading `/`
and the `/=` delimiter; backslash escapes inside the pattern are honoured, so `\/`
matches a literal slash (`parse_match_spec`, `src/ext/hooks.c:155`). The regex is
compiled with `REG_EXTENDED | REG_NEWLINE`. A bad regex or a malformed spec logs a notice
and the hook is skipped — zyterm still starts.

```sh
# Beep and log when the firmware panics:
zyterm --on-match '/PANIC|Oops|Kernel panic/=notify-send "device panic"' /dev/ttyUSB0
```

### Two action kinds

An ACTION is dispatched one of two ways (`fire_hook`, `src/ext/hooks.c:138`):

1. **`send:` prefix — inject bytes onto the wire.** Everything after `send:` is run
   through the *same* escape expander as F-key macros (`\r \n \t \\ \xNN`). This is the
   way to auto-respond to a prompt:

   ```sh
   # When the bootloader prints "login:", send the username:
   zyterm --on-match '/login:/=send:root\r' /dev/ttyUSB0

   # Send an init string as soon as we connect:
   zyterm --on-connect 'send:AT+INIT\r' /dev/ttyUSB0
   ```

2. **Anything else — shell out.** The action is run as `/bin/sh -c <action>` in a forked
   child. stdin is redirected to `/dev/null` (so the hook can't steal your keystrokes);
   stdout/stderr stay attached to your terminal, so the command's output appears inline.
   zyterm does **not** wait on the child — it is reaped opportunistically by the main
   loop's `hooks_reap` (`src/ext/hooks.c:255`). Up to 32 outstanding children are tracked.

### Environment passed to shell hooks

A shelled-out hook receives these variables (`run_shell_action`, `src/ext/hooks.c:92`):

| Variable         | Value                                                      |
|------------------|------------------------------------------------------------|
| `ZYTERM_PORT`    | the device path or URL we're connected to                  |
| `ZYTERM_BAUD`    | serial baud as a decimal string (`0` for socket transports)|
| `ZYTERM_LINE`    | the matched RX line, NUL-terminated, capped at 1 KiB (match events only) |
| `ZYTERM_MATCH`   | currently the full matched line (capture-group extraction is a future addition) |
| `ZYTERM_PATTERN` | the original regex source text (match events only)         |

```sh
# Capture a CSV row to a file every time a sensor line arrives:
zyterm --on-match '/^DATA,/=printf "%s\n" "$ZYTERM_LINE" >> readings.csv' /dev/ttyACM0
```

> `ZYTERM_LINE` and `ZYTERM_MATCH` hold the same value today; `ZYTERM_MATCH` exists so a
> future release can populate it with the first capture group without breaking the
> environment contract.

### Hooks and the trust boundary

Hooks run arbitrary shell commands. Only register hooks whose actions you control. The
related concern for the HTTP bridge — that anyone who can reach the loopback port can
inject TX — is covered in the [recipes security note](recipes.md#the-http-browser-view)
and [SECURITY.md](../../SECURITY.md).

---

## Putting it together

A typical board profile combines all three. Save it once:

```sh
zyterm --baud 115200 --map-out crlf \
       --macro F1='\x03' --macro F2='reboot\r' \
       --on-connect 'send:\r' \
       --on-match '/PANIC/=notify-send "panic on $ZYTERM_PORT"' \
       --port-glob '/dev/ttyUSB*' \
       --profile-save lab
```

Then every session is just:

```sh
zyterm --profile lab
```

…and you can tweak the macros or line-ending maps live by editing
`~/.config/zyterm/lab.conf` while it's running.

For hardware-oriented end-to-end workflows, continue to [Recipes](recipes.md).
