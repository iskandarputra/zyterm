# Chapter 3 — Logging & capture

Three different things, often confused:

| Want                                           | Use                | Output                  |
| ---------------------------------------------- | ------------------ | ----------------------- |
| Plain-text log of RX (and optionally TX)       | `-l file`          | UTF-8 text              |
| Headless capture (no UI), stop after N seconds | `--dump N -l file` | UTF-8 text              |
| Sharable, replay-able demo (with timing)       | `--rec file.cast`  | asciinema v2 JSON-lines |
| Replay a previous capture through the live UI  | `--replay file`    | n/a (reads only)        |

These are independent and can be combined.

## Plain-text logging

The most common pattern. Every received line is appended with a
millisecond timestamp:

```sh
./zyterm /dev/ttyUSB0 -b 115200 -l boot.log
```

Sample `boot.log`:

```
[12:03:14.001] *** Booting Zephyr OS build v3.5.0 ***
[12:03:14.044] [00:00:00.001,000] <inf> main: Hello world
[12:03:14.052] uart:~$
```

### Toggle logging from inside the session

Press `Ctrl+A l`. zyterm starts (or stops) writing to an auto-named
file in the current directory:

```
zyterm-20260114-001.txt
zyterm-20260114-002.txt
```

The HUD flashes the chosen path. Toggling it off closes the file
cleanly.

### Also log TX

Add `--tx-ts`. Outgoing bytes are logged with `-> ` prefix and a
timestamp, so you can correlate stimulus with response:

```sh
./zyterm /dev/ttyUSB0 -l session.log --tx-ts
```

```
[12:05:01.232] uart:~$
[12:05:03.117] -> version
[12:05:03.119] version
[12:05:03.121] zephyr 3.5.0
```

### Rotate at a size threshold

```sh
./zyterm /dev/ttyUSB0 -l boot.log --log-max-kb 1024
```

When `boot.log` exceeds 1024 KiB, zyterm renames it to `boot.log.1`
(overwriting any previous `.1`) and starts a fresh `boot.log`. There's
only one rotation slot — this is intentionally cheap. If you need
more retention, copy `boot.log.1` aside on a cron / watcher.

## Headless capture (`--dump`)

No UI at all. Useful for CI, overnight soak tests, or producing a
canned capture for someone to replay.

```sh
./zyterm /dev/ttyACM0 --dump 60 -l ci.log --no-color
```

- `60` = run for 60 seconds, then exit cleanly. `0` = forever
  (until Ctrl+C / SIGTERM).
- `--no-color` strips the ANSI palette zyterm normally applies to
  log-level prefixes (`<inf>`, `<dbg>`, etc.) — you almost always
  want this when piping to other tools.

Combine with `--watch` for a CI gate:

```sh
./zyterm /dev/ttyACM0 --dump 30 -l ci.log --watch 'PANIC|ASSERT' --watch-beep
grep -E 'PANIC|ASSERT' ci.log && exit 1
```

## Log formats

The default is human-readable text. Two alternatives via `--log-format`:

| Mode   | Each line is...                                            | When to use                             |
| ------ | ---------------------------------------------------------- | --------------------------------------- |
| `text` | `[ts] line` (timestamped, ANSI-stripped)                   | Default. Greppable.                     |
| `json` | `{"t":12.345,"dir":"rx","data":"..."}` (one JSON per line) | Feeding `jq`, log shippers, dashboards. |
| `raw`  | The exact bytes as received (no timestamps, no escaping)   | Reproducing protocol bugs bit-for-bit.  |

```sh
./zyterm /dev/ttyUSB0 -l rx.jsonl --log-format json
```

You can also flip formats live with `Ctrl+A j`.

## asciinema cast (`--rec`)

For sharing demos:

```sh
./zyterm /dev/ttyUSB0 --rec session.cast
# ... do stuff ...
# Ctrl+A q

asciinema play session.cast       # locally
asciinema upload session.cast     # to asciinema.org (account needed)
```

The cast captures both RX and the rendered UI with original timing.
Combine with `-l` if you also want a plain-text record.

## Replay (`--replay`)

Push an old log back through the live UI:

```sh
./zyterm --replay boot.log                # real-time
./zyterm --replay boot.log --replay-speed 4   # 4× faster
./zyterm --replay boot.log --replay-speed 0   # as fast as possible
```

This works on whatever zyterm wrote — text logs **and** asciinema
casts. No serial device is opened; the binary just streams the file
through the renderer.

## Files & paths

- Log files are written to whatever path you pass — relative paths are
  relative to the cwd at startup.
- The `Ctrl+A l` auto-name uses the cwd and the local date.
- All log writes are line-buffered (`fsync` is **not** called on every
  line — the kernel decides). On crash you may lose the last few
  hundred bytes; if that's a problem, run `--log-format raw` and let
  the kernel hand the bytes to a separate consumer (`tail -F`, etc.).

## Next

See [chapter 4 — Profiles & hot-reload](04-profiles.md).
