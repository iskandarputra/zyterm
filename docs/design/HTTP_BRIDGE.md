# Design: the HTTP / SSE / WebSocket bridge

How zyterm's built-in web bridge was designed, and why. Rationale layer — the
line-by-line walk-through is in [../reference/INTERNALS.md](../reference/INTERNALS.md);
the don't-regress rules are [../invariants/INVARIANTS.md](../invariants/INVARIANTS.md)
§3 (never block the loop) and §7 (network bridge trust boundary); the threat model is
[../../SECURITY.md](../../SECURITY.md).

Source of truth: `src/net/http.c`.

## Goals and the self-contained constraint

The bridge exists so the live serial stream can be watched (and driven) from a browser:
a built-in dashboard, an event feed for scripts, a Prometheus scrape, and a state JSON
for tooling. The hard design constraint is the single-binary, libc-only ethos
([../reference/ARCHITECTURE.md](../reference/ARCHITECTURE.md)): no libwebsockets, no
libmicrohttpd, no TLS library. Everything is plain BSD sockets plus a small,
self-contained SHA-1 and base64 for the one place the WebSocket spec demands them — the
upgrade handshake (`src/net/http.c:35-135`). The whole server is a few hundred lines
and adds zero runtime dependencies.

It is started with `--http <port>` (and optionally `--webroot`, `--http-cors`,
`--metrics`); see [../reference/CLI.md](../reference/CLI.md).

## Endpoints

`classify_request()` (`src/net/http.c:838`) dispatches a fully-read request:

| Method + path | Behaviour | Source |
| --- | --- | --- |
| `GET /` (and `/index.html`) | Serves the built-in `kIndex` dashboard, or a file from `--webroot` if set | `:919-940` |
| `GET /stream` (alias `/api/stream`) | Promotes the connection to an SSE stream of RX bytes (and `tx`/`input` events) | `:850-860` |
| `GET /ws` | RFC 6455 WebSocket upgrade; text frames of RX | `:861-886` |
| `GET /metrics` | Prometheus text snapshot (rx/tx bytes, lines, CRC errors) | `:887-899` |
| `GET /api/state` (alias `/api/info`) | JSON device + counters snapshot | `:900-906` |
| `POST /tx` (alias `POST /api/send`) | Writes the request body to the serial line | `:907-918` |
| `OPTIONS *` | CORS preflight | `:844-848` |
| other `GET` | webroot file or `404` | `:919-940` |
| anything else | `405 Method Not Allowed` | `:941` |

The dashboard (`kIndex`, `:220-598`) is a single self-contained HTML page: it opens an
`EventSource('/stream')`, renders RX with a small client-side ANSI-SGR-to-HTML parser,
batches DOM updates via `requestAnimationFrame`, mirrors TX echo and the terminal input
line, and `POST`s `/tx` to send. It auto-reconnects the SSE on error.

## The non-blocking accept + HC_NEW pump (slowloris-safe)

The bridge runs *inside the single-threaded event loop*. The cardinal rule
([INVARIANTS §3](../invariants/INVARIANTS.md)) is that nothing in that loop may block —
a stalled HTTP client must never freeze the serial UI. The connection state machine is
built entirely around that rule.

The listening socket is created loopback-only (`INADDR_LOOPBACK`, `:189`) and set
`O_NONBLOCK` (`:199-200`). Connections live in a fixed table of `HC_MAX` = 16 slots
(`:159-160`), each tagged `HC_NEW`, `HC_SSE`, or `HC_WS` (`:150`).

`http_tick()` (`:985`), called every loop iteration, does two non-blocking things:

1. **Drain accepts.** `accept_one()` (`:818`) loops `accept4(..., SOCK_NONBLOCK)` until
   `EAGAIN`. A freshly-accepted fd goes into an `HC_NEW` slot with its own
   `req_buf`/`req_len` and an `accepted_at` timestamp. If all 16 slots are full it is
   closed politely so the kernel backlog can drain (`:831-833`).
2. **Pump in-flight requests.** For each `HC_NEW` slot, `hc_pump_new()` (`:949`) reads
   whatever is available non-blockingly into the per-connection buffer; on seeing the
   `\r\n\r\n` header terminator it calls `classify_request()`.

The header read being **per-connection and non-blocking** is the slowloris defence, and
it was a deliberate rewrite. The earlier implementation read the request synchronously
inside accept with a `20 × usleep(10ms)` busy-wait — a single slow client could freeze
the entire UI and serial loop for up to 200 ms per connection (`:137-146`). Now a slow
or stalled client simply occupies an `HC_NEW` slot until `HC_HEADER_TIMEOUT_S` = 5 s
elapses, at which point `hc_pump_new()` drops it (`:977-982`); the loop never waits on
it. Oversized headers (≥ `HC_REQ_CAP` = 4096 B) get a `431` and are closed (`:952-957`).

Once classified, SSE and WS slots stay open for streaming; one-shot responses
(`/metrics`, `/api/state`, `/tx`, files, errors) are written and the slot closed
immediately.

One safety detail worth noting in the design: `snprintf_len()` (`:166`) clamps every
`snprintf` return value to the bytes actually present in the buffer before it is used as
a write count. `snprintf` returns the *would-be* length on truncation, and using that
directly as a length would make the kernel read past the stack buffer and emit
attacker-influenced bytes — so every header write goes through this clamp.

## Broadcast: SSE vs WebSocket, and the byte budget

RX bytes are pushed to all streaming clients by `http_broadcast()` (`:1017`); TX echo
and input-line mirroring go through `http_broadcast_tx()` (`:1042`) and
`http_notify_input()` (`:1062`). SSE frames are base64 in a `data:` event; WS frames are
RFC 6455 text frames built by `ws_frame_text()` (`:995`).

Writes are non-blocking and **drop-on-full, not block**: on `EAGAIN` the event is
dropped rather than stalling the loop; any other error (or a short write that can't be
queued) closes the SSE connection (`:1028-1035`). This is the right posture for a
diagnostic feed — a slow browser loses events, it never wedges the serial path.

## Open items and defects

The broadcast path has two recorded defects (see
[../tracking/KNOWN_ISSUES.md](../tracking/KNOWN_ISSUES.md)):

- **ZT-007** (🟠 medium, `src/net/http.c:1020`) —
  [issue](../tracking/issues/ZT-007-http-broadcast-truncates-4k.md). `http_broadcast`
  caps each broadcast at 4096 B (`chunk = n > 4096 ? 4096 : n`) with no loop, so an RX
  burst larger than 4 KiB is silently truncated in the web view. Fix: loop in ≤4096-byte
  segments.
- **ZT-009** (🟠 medium, `src/net/http.c:1037`) —
  [issue](../tracking/issues/ZT-009-ws-broadcast-ignores-errors.md). The WS path calls
  `ws_frame_text()` and ignores its write result, unlike the SSE path which checks and
  closes on error. Dead WS peers are never detected, and a partial frame corrupts the
  stream. Compounding it, **ZT-017** (⚪ low, `:1036`) notes those dead WS slots are never
  reaped, so 16 ungraceful disconnects exhaust every slot and DoS the bridge. Fix:
  mirror the SSE check-and-close on WS writes.

## Security posture (read this before exposing the bridge)

The bridge binds **loopback only** (`INADDR_LOOPBACK`, `src/net/http.c:189`), so it is
not reachable from the network by default. But within that boundary it is
**completely unauthenticated** — there is no token, no password, no session. This is the
local-IPC trust boundary called out in
[INVARIANTS §7](../invariants/INVARIANTS.md) and detailed in
[../../SECURITY.md](../../SECURITY.md). Treat "anything that can make HTTP requests to
127.0.0.1" as fully trusted, because it is. The relevant defects:

- **ZT-004** (🔴 high, `src/net/http.c:907`) —
  [issue](../tracking/issues/ZT-004-unauth-http-tx-csrf.md). `POST /tx` / `POST /api/send`
  writes the request body straight to the serial line with no authentication. Because
  the body is a CORS *simple request* and (with `--http-cors`) the server returns
  `Access-Control-Allow-Origin: *`, a malicious web page the operator merely *visits* —
  or a DNS-rebinding attacker — can drive commands onto the device. Fix direction: a
  bearer token plus `Origin`/`Host` validation, and never a wildcard CORS on
  state-changing routes.
- **ZT-013** (🟠 medium, `src/net/http.c:861`) — the `/ws` upgrade does **no `Origin`
  check**, so any cross-origin site can open a WebSocket and read the live RX stream.
  Fix: an `Origin` allowlist plus the same token.
- **ZT-017** (⚪ low, `src/net/http.c:1036`) — the WS-slot leak above is also a
  denial-of-service vector for the bridge.

Until ZT-004 and ZT-013 are fixed, do not enable `--http` on a shared or multi-user host,
and never combine `--http-cors` `*` with `POST /tx` on anything you care about. See
[../plans/RELIABILITY_HARDENING.md](../plans/RELIABILITY_HARDENING.md) for the fix order.

Also note: `--http` parses its port with `atoi()` and no range check (**ZT-020**,
`src/main.c:586`) — pass a valid 1–65535 port.

---

_Last updated: 2026-06-03._
