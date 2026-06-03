# FAQ — common questions and troubleshooting

Answers grounded in how zyterm actually behaves today. Each answer points to the
canonical reference or guide for detail. If something here contradicts an older
doc, this page (and the cited source) wins.

Quick links: [CLI flags](CLI.md) · [Keybindings](KEYBINDINGS.md) ·
[Getting started](../guide/getting-started.md) ·
[Logging and capture](../guide/logging-and-capture.md) ·
[Troubleshooting guide](../guide/troubleshooting.md) ·
[Known issues](../tracking/KNOWN_ISSUES.md).

---

## Is my command history saved between sessions?

**No. History is in-memory only and is lost when zyterm exits.** There is no
`~/.zyterm_history`, `~/.zyterm/history`, or similar file — the history ring
(`history_push`/`history_at`) never touches disk. The `Up`/`Down` arrows recall
entries only within the current session. This is a deliberate decision; see
[ADR-0006: in-memory history and bookmarks](../decisions/0006-in-memory-history-and-bookmarks.md).

If you need a durable record of what was sent and received, enable logging
(`-l/--log <file>` or `Ctrl+A l`) — see
[Logging and capture](../guide/logging-and-capture.md).

## Are my bookmarks saved?

**No — bookmarks are also in-memory only.** `Ctrl+A +` adds a bookmark at the
current scrollback offset and `Ctrl+A [` lists them, but they live in a fixed
in-process array (`src/ext/bookmarks.c`) and are gone on exit. Same rationale as
history: [ADR-0006](../decisions/0006-in-memory-history-and-bookmarks.md).

## Where do my profiles live, then?

**Profiles _are_ persisted**, at `~/.config/zyterm/<name>.conf` (or
`$XDG_CONFIG_HOME/zyterm/<name>.conf` if that variable is set —
`src/ext/profile.c:40`). They are **not** under `~/.zyterm/profiles` (an older
doc claim that was never true).

- Load one with `--profile <name>`.
- Save the current settings with `--profile-save <name>`.
- A loaded profile is watched with inotify and hot-reloads when you edit the
  file.

See [CLI flags](CLI.md) for the full profile flag set.

> **Caution.** Editing a watched profile that contains a `device=` key can
> currently crash zyterm on reload (a pointer-ownership bug, **ZT-001**, in
> [KNOWN_ISSUES](../tracking/KNOWN_ISSUES.md)). Until that is fixed, prefer
> setting the device on the command line rather than in a hot-reloaded profile.

## How do I connect to a remote serial port? Does `rfc2217://` work?

**`rfc2217://` is not implemented.** Passing it makes zyterm exit immediately
with a message telling you what to do instead (`src/serial/transport.c:94`):

```
zyterm: rfc2217:// is not yet implemented;
for now use 'ser2net' in raw mode and connect with tcp://...
```

The supported network transports are **`tcp://host:port`** and
**`telnet://host:port`**. The recommended setup is to run `ser2net` in **raw**
mode on the remote host and connect with `tcp://`:

```bash
zyterm --baud 115200 tcp://192.168.1.50:5000
```

This is a deliberate deferral — see
[ADR-0005: rfc2217 deferred](../decisions/0005-rfc2217-deferred.md).

## How does reconnect / hot-plug work?

Reconnect is **on by default** (`--reconnect`; disable with `--no-reconnect`).
When the device disappears (unplug, server drop), zyterm shows a reconnect
dialog and retries on a timer (`src/ext/reconnect.c`). You can also force a
manual reconnect at any time with `Ctrl+A r`.

For USB-serial adapters that get a new device node when re-plugged, give zyterm
a way to re-find the port so it does not chase a stale path:

- `--port-glob <pattern>` — e.g. `--port-glob '/dev/ttyUSB*'`
- `--match-vid-pid <VID:PID>` — match by USB vendor/product (hex)

On each reconnect attempt these are re-resolved (`reconnect_attempt`,
`src/ext/reconnect.c:41`), so the same physical adapter is found even if its
node number changed. See [CLI flags](CLI.md) and ADR-0004 for the on-by-default
rationale.

> **Note.** A failed `Ctrl+A A` autobaud can currently leave the connection in
> a state where automatic reconnect does not fire (**ZT-005**,
> [KNOWN_ISSUES](../tracking/KNOWN_ISSUES.md)). If that happens, press
> `Ctrl+A r` to reconnect manually.

## Why is the reader thread opt-in instead of always on?

The threaded reader (`--threaded`) drains the serial device on a dedicated
worker into a lock-free SPSC ring while the main thread runs the render/log
pipeline (`src/loop/rx_thread.c`). It smooths jitter during **high-baud**
bursts, but at low baud rates it is strictly a pessimization — it adds a syscall
per byte for no benefit — so it is left **off by default and enabled explicitly**
(`src/loop/rx_thread.c:10`). The default single-threaded `poll(2)` loop is
correct for the common case; reach for `--threaded` only when you are pushing
high data rates and seeing dropped or choppy RX.

## How do I capture a session, and how do I replay it?

Three mechanisms, all in [Logging and capture](../guide/logging-and-capture.md):

- **Log file** — `-l/--log <file>` (or `Ctrl+A l`) appends RX/TX with
  timestamps. Format is `--log-format <text|json|raw>`; rotate with
  `--log-max-kb <N>`.
- **asciinema cast** — `--rec <file>` records the terminal session as a cast
  v2 file that plays back in `asciinema play` (`src/log/record_cast.c`).
- **Headless dump** — `--dump <sec>` captures for N seconds (0 = forever) with
  no interactive UI.

To **replay** a previously captured file through the UI:

```bash
zyterm --replay session.log --replay-speed 2.0
```

`--replay-speed` is a multiplier (default `1.0`; `0` = as fast as possible).
Replay needs no device. See [CLI flags](CLI.md) for the exact flags.

## Where do log files go?

- With **`-l/--log <file>`** you choose the path explicitly.
- With **`Ctrl+A l`** zyterm auto-names the file `zyterm-YYYYMMDD-NNN.txt` and
  writes it to the **current working directory** (`src/loop/input.c:153`),
  picking the next free `NNN` for today.
- Rename the active log without stopping it with `Ctrl+A n`.

Diagnostic and status lines (`<dbg>`, `<inf>`, stats) are part of the in-app
output stream, not a separate log file; mute them with `Ctrl+A D` / `Ctrl+A I`
(or `--mute-dbg` / `--mute-inf`). The Prometheus `--metrics <path>` exporter
writes to a UNIX socket you specify, not to a log file.

## Why does pasted or device output sometimes change my terminal title or clipboard?

zyterm currently writes device RX to your terminal with minimal escape filtering
(only `\r` is stripped). A hostile or buggy device can therefore emit escape
sequences that affect your terminal (title, and OSC 52 clipboard writes). This
is a known security gap (**ZT-003**) tracked in
[KNOWN_ISSUES](../tracking/KNOWN_ISSUES.md). Until it is fixed, be cautious
pointing zyterm at untrusted endpoints, and avoid exposing the HTTP bridge
(`--http`) on untrusted networks (see ZT-004/ZT-013 there).

## I saw "OSC 8 hyperlinks", "fuzzy finder", or "multi-pane" mentioned — do they work?

No. These appear in the UI or in older notes but are not functional today:

- **OSC 8 hyperlinks** — the settings toggle flips a flag nothing reads; the
  rewrite routine is never called. **ZT-019.**
- **Fuzzy finder** (`Ctrl+A .`) — non-functional; use `Up`/`Down` history
  instead. **ZT-008.**
- **Multi-pane** — a stub; not wired or keybound.

All tracked in [KNOWN_ISSUES](../tracking/KNOWN_ISSUES.md). See
[Keybindings](KEYBINDINGS.md) for what the keys actually do today.

---

_See also: [CLI](CLI.md) · [Keybindings](KEYBINDINGS.md) ·
[Logging and capture](../guide/logging-and-capture.md) ·
[Troubleshooting](../guide/troubleshooting.md) ·
[ADR index](../decisions/README.md) ·
[Known issues](../tracking/KNOWN_ISSUES.md)._
