# Recipes: real embedded & hardware workflows

Task-oriented recipes for the things you actually do at a bench: surviving a USB
re-plug, fixing line endings, decoding a framed protocol, watching for a string, viewing
a session in a browser, talking to a remote serial server, and comparing two captures.

Each recipe is grounded in the shipping behaviour. Where a feature has a sharp edge it is
called out, not glossed over.

See also: [Getting started](getting-started.md) · [Automation](automation.md) ·
[Logging & capture](logging-and-capture.md) · [CLI reference](../reference/CLI.md) ·
[Troubleshooting](troubleshooting.md).

---

## Hot-plug reconnect (survive a USB re-plug)

USB serial adapters get a fresh `/dev/ttyUSB*` index every time they re-enumerate.
zyterm reconnects on its own — auto-reconnect is **on by default** (`--reconnect`; disable
with `--no-reconnect`). To make reconnect re-find the *device* rather than re-open a stale
path, give it a discovery hint:

```sh
# Re-resolve the first matching glob on every reconnect:
zyterm --port-glob '/dev/ttyUSB*' --baud 115200

# Or pin to a specific adapter by USB vendor:product (hex):
zyterm --match-vid-pid 0403:6001          # FTDI FT232R
zyterm --match-vid-pid 1a86:7523          # CH340
```

On each reconnect zyterm re-runs discovery (`port_rediscover`,
`src/serial/port_discover.c:159`): with `--port-glob` it takes the first matching path;
with `--match-vid-pid` it walks the candidate devices and matches on the kernel's
`idVendor`/`idProduct`. You can combine them — the glob narrows the candidate set and the
VID:PID picks the winner. If nothing matches at startup, zyterm exits with
`no device matched --port-glob / --match-vid-pid` (`src/main.c:714`).

Manual reconnect any time: press `Ctrl+A r`.

> The VID:PID is parsed as hex and each half must be ≤ `0xFFFF` (`src/main.c:636`). Find
> your adapter's IDs with `lsusb`.
>
> See [`design/THREADING_AND_RECONNECT.md`](../design/THREADING_AND_RECONNECT.md) and
> [ADR-0004](../decisions/0004-reconnect-on-by-default.md) for why reconnect is on by
> default and how the loop handles hang-up.

---

## Line-ending fixes

Different firmware expects different end-of-line bytes. zyterm rewrites them in both
directions with `--map-out` (host → device) and `--map-in` (device → host). Modes:
`none` (default), `cr`, `lf`, `crlf`, `cr-crlf`, `lf-crlf` (`src/proto/line_endings.c`).

Common cases:

```sh
# Device wants CRLF on input, you press Enter (LF):  send CRLF.
zyterm --map-out crlf /dev/ttyUSB0

# Device emits bare CR (old Mac-style / some RTOS shells); show it as LF:
zyterm --map-in cr /dev/ttyUSB0

# Device emits CRLF, you want clean single LF lines in the log/terminal:
zyterm --map-in crlf /dev/ttyACM0
```

The translation is streaming and chunk-boundary safe — a CR ending one read and an LF
starting the next are still coalesced correctly (a one-byte "saw CR" latch carries across
calls, `src/proto/line_endings.c:74`). `--map-out crlf` also collapses a user-typed
`CR LF` into a single `CRLF` rather than doubling it.

These keys are also profile-saveable (`map_out` / `map_in`) and hot-reloadable — see
[Automation](automation.md#profiles).

---

## Framing + CRC for a framed protocol

If your device speaks a framed binary protocol rather than a line-oriented shell, point
the decoder at it with `--frame` and (optionally) `--crc`. Verified decoders:

| `--frame`  | Framing                                  |
|------------|------------------------------------------|
| `raw`      | no framing (default)                     |
| `cobs`     | Consistent Overhead Byte Stuffing        |
| `slip`     | SLIP (RFC 1055)                          |
| `hdlc`     | HDLC-style byte-stuffing                  |
| `lenpfx`   | length-prefixed frames                   |

| `--crc`    | Trailing CRC checked & stripped per frame |
|------------|-------------------------------------------|
| `none`     | no CRC (default)                          |
| `ccitt`    | CRC-16-CCITT                              |
| `ibm`      | CRC-16-IBM                                |
| `crc32`    | CRC-32                                    |

```sh
# A SLIP-framed link with a CCITT trailer on each frame:
zyterm --frame slip --crc ccitt /dev/ttyUSB0

# COBS with CRC-32:
zyterm --frame cobs --crc crc32 /dev/ttyACM0
```

When `--crc` is set, the trailing CRC is verified and stripped before each frame is
displayed/logged. Framing and CRC are profile-saveable (`frame` / `crc`) and the enums
hot-reload live. The framing/CRC mode can also be cycled at runtime with `Ctrl+A F`
(framing) and `Ctrl+A K` (CRC).

For the on-wire details and decoder bounds, see
[`design/FRAMING_AND_CRC.md`](../design/FRAMING_AND_CRC.md) and
[INVARIANTS §5](../invariants/INVARIANTS.md).

---

## Watch for a string (and beep)

`--watch <pattern>` highlights lines containing a substring; up to 8 patterns are allowed.
Add `--watch-beep` to ring the terminal bell (BEL, `\007`) on a match
(`src/render/render.c:138`):

```sh
# Flag boot errors in reverse video and beep when one appears:
zyterm --watch ERROR --watch FAIL --watch-beep /dev/ttyUSB0
```

Each watch slot gets its own highlight colour. Combine with `--on-match` from
[Automation](automation.md#event-hooks) when you want to *do* something (run a command,
inject a reply) rather than just see it.

---

## The HTTP browser view

`--http <port>` starts a small built-in web bridge: open the URL in a browser to watch
the live RX stream (via Server-Sent Events / WebSocket) and type bytes back to the device
from a text box. Optionally serve your own static files with `--webroot <dir>` and a
Prometheus scrape endpoint with `--metrics <path>`.

```sh
zyterm --http 8080 /dev/ttyUSB0
# then open http://127.0.0.1:8080/
```

The server **binds to loopback only** — `127.0.0.1` (`src/net/http.c:189`) — and logs
`http bridge on http://127.0.0.1:<port>/`.

> ### ⚠ Security: the bridge is unauthenticated
>
> The `POST /tx` and `POST /api/send` routes write their request body straight to the
> serial line with **no authentication and no Origin/CSRF check** (`src/net/http.c:907`).
> Anyone who can reach the port — including a malicious web page in your browser via a
> CORS simple-request or DNS-rebind — can execute commands on the attached device. This
> is the project's local-IPC trust boundary.
>
> Treat the bridge as a single-user, trusted-machine convenience. **Do not** expose the
> port beyond loopback, **do not** add `--http-cors` (which sends
> `Access-Control-Allow-Origin: *`) on a machine you don't fully trust, and don't leave
> the bridge running unattended on a device that can do harm.
>
> Tracked as [ZT-004](../tracking/issues/ZT-004-unauth-http-tx-csrf.md) and
> [ZT-013](../tracking/KNOWN_ISSUES.md) (WebSocket Origin). See
> [SECURITY.md](../../SECURITY.md), [INVARIANTS §7](../invariants/INVARIANTS.md), and
> [`design/HTTP_BRIDGE.md`](../design/HTTP_BRIDGE.md).

`--http` currently parses the port with `atoi` and does not range-validate it
([ZT-020](../tracking/KNOWN_ISSUES.md)); pass a sane port in `1`–`65535`.

---

## tcp:// and telnet:// bridges

zyterm can talk to a *remote* serial server the same way it talks to a local tty, by
giving it a URL instead of a device path (`src/serial/transport.c`):

```sh
# Raw TCP to a ser2net port in raw mode:
zyterm tcp://lab-pi.local:23000 -l boot.log

# A Telnet server (IAC escaping handled automatically):
zyterm telnet://192.168.1.50:2000
```

`tcp://` opens a plain socket with sensible keep-alive defaults. `telnet://` additionally
escapes outgoing `0xFF` and strips incoming Telnet IAC sequences so a real telnet server
isn't confused. Because the runtime already drives `c->serial.fd` through
`poll`/`read`/`write`, sockets work everywhere a tty would. For socket transports
`ZYTERM_BAUD` is reported as `0` to hooks (there is no line rate to report).

> **`rfc2217://` is not implemented.** It is an intentional stub that exits with an
> actionable message: use a ser2net port in *raw* mode plus `tcp://` instead
> (`transport_open` in `src/serial/transport.c`). See
> [ADR-0005](../decisions/0005-rfc2217-deferred.md).

---

## File transfer: XMODEM / YMODEM / ZMODEM

zyterm ships file-transfer engines: a native XMODEM-CRC (128-byte blocks, CRC-16-CCITT,
`src/proto/xmodem.c`), a native YMODEM, and ZMODEM via the system `lrzsz` tools (`sz`/`rz`
are spawned and relayed over the serial fd, `src/proto/xmodem.c:376`).

> **Honest status:** these transfer engines are exposed through the **embedding API**
> (`xmodem_send`/`xmodem_receive`/`ymodem_send`/`ymodem_receive`/`zmodem_send`/
> `zmodem_receive` in `include/zyterm/internal/proto.h`). In the **standalone TUI there is
> currently no CLI flag or keybinding that triggers them** — they have no interactive
> entry point in the shipped binary. If you embed zyterm, call these functions directly
> (the device must already be open; baud/parity/etc. are your responsibility). For
> ZMODEM, the host needs `lrzsz` installed (`sz`, `rz` on `PATH`).

When invoking the ZMODEM relay programmatically, `zmodem_receive(c, dir)` temporarily
`chdir`s into `dir` for the duration of the `rz` run and restores the cwd afterward, and a
wedged `sz`/`rz` is escalated SIGTERM → SIGKILL so it can't hang the host.

If you only need a quick interactive byte push from the standalone TUI, use an F-key macro
or an `--on-match` `send:` action (see [Automation](automation.md)) — that is the
supported way to inject bytes today.

---

## Diffing two captures

After a firmware change, compare two captured logs to find the first lines that diverged:

```sh
zyterm --diff before.log after.log
```

`--diff <a> <b>` reads two RAW or TEXT zyterm logs and prints a unified-style summary of
the mismatching lines, then exits (`diff_run`, `src/ext/diff.c:19`). It does not open a
device, so it works anywhere. Pair it with `-l <file>` capture (see
[Logging & capture](logging-and-capture.md)) to produce the inputs.

---

For configuring profiles, macros, and hooks used in these recipes, see
[Automation](automation.md). When something misbehaves, see
[Troubleshooting](troubleshooting.md).
