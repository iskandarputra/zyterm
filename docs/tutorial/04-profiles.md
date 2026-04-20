# Chapter 4 — Profiles & hot-reload

A profile is a small text file that pre-fills CLI flags, lives at
`~/.config/zyterm/<name>.conf`, and reloads itself automatically when
you edit it.

## The smallest possible profile

```ini
# ~/.config/zyterm/myboard.conf
device = /dev/ttyUSB0
baud   = 115200
```

Then:

```sh
zyterm --profile myboard
```

That's identical to typing `zyterm /dev/ttyUSB0 -b 115200`.

## Every supported key

These are the **only** keys the parser understands. Anything else is
silently ignored.

| Key          | Type                                                       | Equivalent CLI flag              |
| ------------ | ---------------------------------------------------------- | -------------------------------- |
| `device`     | path or URL                                                | positional `<DEVICE>`            |
| `baud`       | integer (1..20000000)                                      | `-b`                             |
| `data_bits`  | `5` \| `6` \| `7` \| `8`                                   | `--data`                         |
| `parity`     | `n` \| `e` \| `o`                                          | `--parity`                       |
| `stop_bits`  | `1` \| `2`                                                 | `--stop`                         |
| `reconnect`  | `true` \| `false` (also `1`/`0`/`yes`/`no`)                | `--reconnect` / `--no-reconnect` |
| `osc52`      | bool                                                       | `--osc52` / `--no-osc52`         |
| `frame`      | `raw` \| `cobs` \| `slip` \| `hdlc` \| `lenpfx`            | `--frame`                        |
| `crc`        | `none` \| `ccitt` \| `ibm` \| `crc32`                      | `--crc`                          |
| `log_format` | `text` \| `json` \| `raw`                                  | `--log-format`                   |
| `map_out`    | `none` \| `cr` \| `lf` \| `crlf` \| `cr-crlf` \| `lf-crlf` | `--map-out`                      |
| `map_in`     | (same set)                                                 | `--map-in`                       |

> **Note.** Older docs claimed keys like `log-dir`, `watch`, `macro`,
> `tx-ts`, etc. were profile keys. They are **not**. If you want
> macros or watch patterns from a profile, you currently need a small
> wrapper script (see _Wrapper recipe_ below).

### Syntax rules

- One `key = value` per line.
- `#` and `;` start a comment (full-line and end-of-line both fine).
- Whitespace around `=` is ignored.
- Boolean values: `true` / `false` / `1` / `0` / `yes` / `no` (case-insensitive).
- Values are **not** quoted; trailing whitespace is stripped.
- Unknown keys: silently ignored. Misspell `boud=`, get default 115200.

A fully populated example:

```ini
# ~/.config/zyterm/zephyr-fpga.conf
device      = /dev/ttyUSB0
baud        = 921600
data_bits   = 8
parity      = n
stop_bits   = 1
reconnect   = true
osc52       = true
frame       = raw
crc         = none
log_format  = text
map_out     = lf       # send LF only (default)
map_in      = none     # don't rewrite incoming
```

## Saving the current settings

After tuning a session interactively, snapshot it to a profile:

```sh
zyterm /dev/ttyUSB0 -b 921600 --profile-save zephyr-fpga
```

zyterm starts normally. When it exits, it writes
`~/.config/zyterm/zephyr-fpga.conf` containing the **runtime-derived**
values for every supported key listed above.

Open the file and edit by hand for finer control.

## Hot reload

This is the killer feature. While zyterm is running with
`--profile <name>`, a watcher tracks the profile file. On any
modification:

1. The watcher coalesces events for **200 ms** (so a Vim swap-file
   shuffle counts as one change, not three).
2. The new file is parsed.
3. **Runtime-safe** keys are applied immediately (`map_out`, `map_in`,
   `osc52`, `log_format`, `frame`, `crc`, `reconnect`).
4. **Connection-affecting** keys (`device`, `baud`, `data_bits`,
   `parity`, `stop_bits`) are stashed and applied on the next reconnect
   — they don't hot-swap a live serial fd.
5. Status bar flashes the reload event.

You'll feel this immediately if you `:wq` from Vim while zyterm is
showing a stream — the line ending mode flips and the next TX uses
the new value.

### How the watcher works (so you can debug it)

- It uses `inotify` on the **parent directory** (not the file
  directly). This is what makes editor swap-file workflows safe — Vim
  writes to `.profile.conf.swp`, then atomically renames it over the
  real file. A direct file watch would miss the rename; a parent-dir
  watch sees `IN_MOVED_TO` and reloads.
- Watched events: `IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE`.
- The 200 ms debounce is hard-coded in `src/ext/profile_watch.c`.

### Why `device`/`baud` don't hot-swap a live connection

Changing baud while the line is mid-byte would corrupt traffic, and
changing the device would orphan the in-flight TX queue. Instead, the
new values take effect on the next physical reconnect — either by
unplugging the cable, or pressing `Ctrl+A r` to force one.

## Wrapper recipe (for things profiles don't cover)

If you want macros, watches, hooks, or `--tx-ts` baked in, write a
shell wrapper:

```sh
#!/usr/bin/env bash
# ~/bin/myboard
exec zyterm --profile myboard \
    --tx-ts \
    --watch 'ERROR|PANIC' --watch-beep \
    --macro F1='version\r' \
    --macro F2='reboot\r' \
    --on-match '/PANIC/=echo "$(date +%T) $ZYTERM_LINE" >> ~/panics.log' \
    "$@"
```

```sh
chmod +x ~/bin/myboard
myboard
myboard -b 230400        # extra args still flow through
```

## Inspecting / removing profiles

```sh
ls ~/.config/zyterm/                # list
cat ~/.config/zyterm/myboard.conf   # inspect
rm  ~/.config/zyterm/myboard.conf   # delete
```

There's no internal "profile list" command — they're just files.

## Next

See [chapter 5 — Event hooks & automation](05-hooks.md).
