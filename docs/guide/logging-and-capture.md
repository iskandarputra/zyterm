# Logging and capture

zyterm can write everything it receives to disk, capture headlessly without a
UI, replay a capture back through the interface, and record a pixel-faithful
asciinema cast of a whole session. This page covers all four, plus log formats,
rotation, and timestamps.

Unlike scrollback and history, **logs are real files** — they persist after you
quit. Command history and bookmarks do not; they live in memory only (see
[decisions/0006-in-memory-history-and-bookmarks.md](../decisions/0006-in-memory-history-and-bookmarks.md)).

For the full flag grammar see [reference/CLI.md](../reference/CLI.md).

## Logging to a file

Append received bytes to a log file with `-l`/`--log`:

```sh
./zyterm /dev/ttyUSB0 -l boot.log
```

The file is opened `O_CREAT | O_APPEND`, so re-running against the same path adds
to it rather than truncating (`src/main.c:720`). You can also toggle logging on
and off mid-session with `Ctrl+A l`; when you turn it on that way, zyterm picks
an auto-generated name for the current day, `zyterm-YYYYMMDD-NNN.txt`, choosing
the next free `NNN` (`src/loop/input.c:153`). `Ctrl+A n` renames the active log
file in place.

The HUD's logging settings page (`Ctrl+A o`, page 4) shows the live log status,
file name, format, rotation threshold, TX-timestamp state, and bytes written so
far.

## Log formats

`--log-format` selects the encoding; the default is `text`
(`src/main.c:567`).

| Format | What lands in the file |
| --- | --- |
| `text` | Human-readable RX, with a `[YYYY-MM-DD HH:MM:SS.mmm]` timestamp emitted at the start of each line (`src/log/logio.c:66`). This is the default and the format the auto-namer's `.txt` suffix implies. |
| `json` | NDJSON / JSONL — one object per RX (and TX, and notice) event: `{"ts":"...Z","dir":"rx","n":42,"b":"..."}` (`src/log/log_json.c:65`). Timestamps are ISO-8601 UTC; control bytes are escaped as `\u00xx`, printable ASCII is verbatim. |
| `raw` | Selectable in the menu and on the CLI; the HUD reports "Raw". |

A practical note grounded in the current implementation: the line-prefixed
timestamped text writer runs for every received chunk regardless of the selected
format (`src/loop/runtime.c:174`), and the JSON writer runs *in addition* when
the format is `json` (`src/render/render.c:346`). So `json` mode yields a file
containing both the NDJSON objects and the text stream. If you need strictly one
encoding, capture `text` (the default) or post-process. Choose `json` when you
want machine-parseable event records and are happy to filter for the `{...}`
lines (`grep '^{' file.log`).

You can also set the format live with `Ctrl+A j` (cycles the format) or from the
logging settings page.

## Rotation

For long-running captures, cap the file size and let zyterm roll it over:

```sh
./zyterm /dev/ttyUSB0 -l serial.log --log-max-kb 4096   # rotate at 4 MB
```

When the active log passes the threshold, zyterm closes it, renames it to
`<file>.1`, and opens a fresh `<file>` (`src/log/logio.c:33`). Only a single
`.1` generation is kept — each rotation overwrites the previous `.1`. If you need
a longer retention chain, rotate externally (logrotate) against a non-rotating
zyterm log.

> Note: rotation currently does not surface `rename()`/`open()` failures, so a
> rotation that fails (e.g. a full disk) can silently lose subsequent output.
> Tracked as ZT-010 in [tracking/KNOWN_ISSUES.md](../tracking/KNOWN_ISSUES.md).

## Timestamps

In `text` format, RX lines are timestamped at each line start automatically. To
also timestamp and tag the bytes *you send*, add `--tx-ts`: TX lines are logged
with a `->` prefix and their own timestamp (`src/log/logio.c:80`). TX is not
logged at all unless `--tx-ts` is set, so an ordinary `--log` file is RX-only.

`--ts` (or `Ctrl+A t`) is a separate, display-only toggle: it shows timestamps in
the on-screen HUD. It doesn't change what the log file contains.

You can also reduce log noise at the source: `--mute-dbg` and `--mute-inf` drop
`<dbg>` and `<inf>` level lines from both the log and scrollback (toggle live
with `Ctrl+A D` / `Ctrl+A I`).

## Headless capture with `--dump`

When you just want to grab output and exit — no UI, scriptable — use `--dump`:

```sh
./zyterm /dev/ttyUSB0 --dump 30 -l boot.log    # capture for 30 seconds
./zyterm /dev/ttyUSB0 --dump 0  -l boot.log    # capture until EOF / Ctrl+C
```

`--dump <sec>` runs a tight read loop with no interactive interface, writing each
chunk to the log (and to the recorder, if `--rec` is set) and echoing it to
stdout (`src/loop/runtime.c:275`). A value of `0` means "forever" — until the
device hangs up or you interrupt it. When it finishes it prints a one-line
summary to stderr: `captured N bytes in T.TTs`. Event hooks still fire on matched
lines during a dump, which makes it convenient for unattended bring-up checks.

## Replaying a capture with `--replay`

Feed a previously captured file back through the full interactive UI as if it
were arriving live:

```sh
./zyterm --replay boot.log
./zyterm --replay boot.log --replay-speed 4     # 4x faster
./zyterm --replay boot.log --replay-speed 0     # as fast as possible
```

Replay needs no device — the serial fd is unused (`src/loop/runtime.c:355`). The
session looks normal (scrollback, search, copy, the HUD all work) but with a
`REPLAY` badge in the banner. `--replay-speed` multiplies the playback rate;
`1.0` is the default (~10 kB/s pacing), `0` removes the inter-byte delay entirely
for a near-instant load (`src/loop/runtime.c:395`). When the file ends, zyterm
keeps the UI alive for inspection; press `Ctrl+A x` to exit.

You can combine replay with `-l` to re-log the replayed stream into a new file —
useful for converting an old capture into a different format.

## Recording an asciinema cast with `--rec`

`--rec` records the *rendered* session — every byte zyterm writes to your
terminal, escape sequences and all — as a standard asciinema cast v2 file:

```sh
./zyterm /dev/ttyUSB0 --rec session.cast
asciinema play session.cast                     # play it back later
```

The tap point is the output buffer flush, so what you record is exactly what you
saw on screen (`src/log/record_cast.c:148`). The cast is line-buffered, so even a
hard `Ctrl+C` leaves a parseable file. The result plays in `asciinema play`,
embeds in the asciinema web player, and round-trips through tools like `agg` to
produce a GIF or SVG.

This is different from `--log` (which records the serial *data*) and from
`--replay` (which re-feeds captured data through the UI): `--rec` records the
*visual* terminal output for sharing or documentation, not the wire bytes.

## What persists, and what doesn't

| Thing | Persisted? |
| --- | --- |
| `--log` / `Ctrl+A l` log files | Yes — real files on disk. |
| `--rec` asciinema cast | Yes — real file on disk. |
| Profiles (`--profile-save`) | Yes — `~/.config/zyterm/<name>.conf`. |
| Command history | No — in memory, lost on exit. |
| Bookmarks | No — in memory, lost on exit. |
| Scrollback | No — in memory ring, lost on exit. |

The in-memory choice for history and bookmarks is deliberate; the reasoning is in
[decisions/0006-in-memory-history-and-bookmarks.md](../decisions/0006-in-memory-history-and-bookmarks.md),
and the [reference/FAQ.md](../reference/FAQ.md) corrects the older (false) claim
that zyterm wrote a `~/.zyterm_history` file.
