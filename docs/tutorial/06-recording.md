# Chapter 6 — Recording & replay

zyterm can record a session to two completely different formats and
replay either of them.

## asciinema cast (`--rec`)

```sh
zyterm /dev/ttyUSB0 -b 115200 --rec demo.cast
# ... do whatever ...
# Ctrl+A q
```

You now have a **v2 asciinema cast** — a JSON-lines file that
preserves every byte and the original timing.

```
{"version":2,"width":120,"height":40,"timestamp":1736812800,"env":{"SHELL":"/bin/bash","TERM":"xterm-256color"}}
[0.012345, "o", "uart:~$ "]
[0.234567, "o", "version\r\nzephyr 3.5.0\r\n"]
...
```

### Playback

```sh
asciinema play demo.cast              # locally, real time
asciinema play --speed 4 demo.cast    # 4× speed
asciinema upload demo.cast            # to asciinema.org
```

`asciinema` itself is not bundled with zyterm — install it from your
distro (`apt install asciinema`, `pip install asciinema`, etc.) or
from <https://asciinema.org>.

### What's recorded

- Every byte the renderer emits to stdout — meaning **the rendered
  view**, including ANSI colors, the input bar, the status bar, etc.
- Original timing (microsecond resolution).
- The dimensions of the terminal at recording time (header `width` /
  `height`).

### What's **not** recorded

- TX bytes you typed (they're rendered if the remote echoes them, so
  in practice they're captured anyway). For a guaranteed TX trace,
  combine with `-l ... --tx-ts`.
- Anything before zyterm started.

### File size

A typical embedded-shell session at 115200 baud produces roughly
~20 KB / minute of cast file (mostly the header + idle gaps). A boot
log dump can reach ~1 MB / minute.

## zyterm's own log (`-l`) is also replay-able

```sh
zyterm /dev/ttyUSB0 -l boot.log
# ... boot the board ...
# Ctrl+A q

zyterm --replay boot.log              # real-time replay through the UI
zyterm --replay boot.log --replay-speed 0   # as fast as possible
zyterm --replay boot.log --replay-speed 8   # 8× speed
```

Unlike asciinema cast playback, this one runs **through zyterm's own
renderer** — so you can:

- Search the replay (`Ctrl+A /`).
- Toggle hex view mid-playback (`Ctrl+A h`).
- Toggle timestamps (`Ctrl+A t`).
- Apply watch highlights:

  ```sh
  zyterm --replay boot.log --watch 'PANIC|ERR' --watch-beep
  ```

This is gold for forensic work — you ship a `boot.log` to a
colleague, they replay it with their preferred filters, no board
required.

### Replay speed semantics

| `--replay-speed` value | Behaviour                                      |
| ---------------------- | ---------------------------------------------- |
| `1.0` (default)        | Real-time playback.                            |
| `0`                    | No delays — flush as fast as the renderer can. |
| `0.5`                  | Half speed (slow motion).                      |
| `4.0`                  | 4× speed.                                      |

Negative values are clamped to `0`.

## Combining recording with logging

There's no conflict — record both:

```sh
zyterm /dev/ttyUSB0 \
    -l demo.log --tx-ts \
    --rec demo.cast
```

You get:

- `demo.log` for `grep`/CI/forensics (RX + TX, timestamped).
- `demo.cast` for showing humans (full visual replay).

## Asciinema cast format reference

The header (line 1) is a single JSON object with required fields
`version`, `width`, `height` and optional `timestamp`, `env`,
`title`, `idle_time_limit`. zyterm writes the conservative set
above.

Subsequent lines are JSON arrays of `[float, "o" | "i", "data"]`:

- The float is seconds since session start.
- `"o"` = output (renderer → terminal). zyterm only emits these.
- `"i"` = input (terminal → app). zyterm does **not** emit these.

If you want to post-process a cast in your own tools, the format is
described at <https://docs.asciinema.org/manual/asciicast/v2/>.

## Next

See [chapter 7 — Recipes & real workflows](07-recipes.md).
