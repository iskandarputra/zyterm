# ADR-0005: `rfc2217://` deferred to a stub

- **Status:** accepted
- **Date:** 2026-06-03

## Context

zyterm can drive a remote serial port over TCP because its runtime already operates on a single
fd via `poll(2)`/`read(2)`/`write(2)`, all of which work unchanged on a connected socket. The
transport layer (`src/serial/transport.c`) detects URL-shaped device strings and opens a socket
instead of a tty:

- **`tcp://host:port`** — raw bytes over TCP (e.g. ser2net in raw mode).
- **`telnet://host:port`** — same, plus minimal Telnet IAC handling: outgoing `0xFF` is escaped
  (`telnet_tx_escape`, `src/serial/transport.c:214`) and incoming IAC sequences are stripped
  (`telnet_rx_filter`, `:163`).

**RFC 2217** is different in kind. The Telnet COM-PORT-OPTION carries serial parameters — baud,
data bits, parity, stop bits, flow-control state, even line/modem signals — as an in-band
negotiation over the Telnet stream. Implementing it correctly means a stateful option negotiator
and a mapping from zyterm's serial settings onto COM-PORT-OPTION sub-commands, with interop
testing against real RFC 2217 servers. That is a meaningfully larger surface than "open a socket
and shovel bytes."

Meanwhile there is a perfectly good escape hatch: run **ser2net in raw mode** and connect with
**`tcp://`**. The serial parameters are then configured server-side, and zyterm needs no
negotiation at all.

## Decision

**`rfc2217://` is a deliberate, actionable stub — not a silent gap.** The scheme is recognized as
a valid URL form (`transport_is_url`, `src/serial/transport.c:49-53`) so it parses, but
`transport_open()` refuses it with a `zt_die` that names the workaround:

```c
if (!strcmp(scheme, "rfc2217")) {
    zt_die("zyterm: rfc2217:// is not yet implemented; "
           "for now use 'ser2net' in raw mode and connect with tcp://%s:%s",
           host, port);
}
```

(`src/serial/transport.c:94-98`). The hook points for a future negotiator are intentionally left
in place; the file header documents the larger negotiation surface as the reason it is stubbed.

## Consequences

- A user who tries `rfc2217://` gets an immediate, specific instruction (ser2net raw + `tcp://`)
  instead of a confusing partial connection or a hang.
- `tcp://` and `telnet://` are the supported network transports today; documentation must describe
  `rfc2217://` as **not implemented** and point at the ser2net workaround — never as a working
  feature. The reference note lives in [`reference/CLI.md`](../reference/CLI.md).
- No new invariant is created: the stub is a leaf that fails fast and touches no shared state.
- Real RFC 2217 support is future work in [`plans/ROADMAP.md`](../plans/ROADMAP.md); a future ADR
  will supersede this one if and when it is implemented.
