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

## Broadcast robustness (fixed)

Two broadcast-path defects are now closed (see
[../tracking/KNOWN_ISSUES.md](../tracking/KNOWN_ISSUES.md)):

- **ZT-007 (fixed)** — [issue](../tracking/issues/ZT-007-http-broadcast-truncates-4k.md).
  `http_broadcast` / `http_broadcast_tx` now loop over the whole payload in ≤4096-byte
  segments for both SSE and WS, so an RX burst larger than 4 KiB is no longer truncated.
- **ZT-009 / ZT-017 (fixed)** —
  [issue](../tracking/issues/ZT-009-ws-broadcast-ignores-errors.md). `ws_frame_text()`
  returns an error and `http_broadcast` closes the WS peer on a failed/partial frame
  (mirroring the SSE check-and-close), so dead peers are reaped and 16 ungraceful
  disconnects no longer exhaust every slot.

## Security posture (read this before exposing the bridge)

The bridge binds **loopback only** (`INADDR_LOOPBACK`, `src/net/http.c`), so it is not
reachable from the network by default. As of the 2026-06 hardening, within that boundary it
is **origin-pinned**, and its write routes can additionally require a bearer token. This is
the local-IPC trust boundary in [INVARIANTS §7](../invariants/INVARIANTS.md), detailed in
[../../SECURITY.md](../../SECURITY.md). What's enforced:

- **ZT-004 (fixed)** — [issue](../tracking/issues/ZT-004-unauth-http-tx-csrf.md).
  `POST /tx` / `POST /api/send` pin `Host`/`Origin` to a loopback literal (rejecting CORS
  simple requests from foreign origins and DNS-rebound hosts with `403`) and, when
  `--http-token` is set, require `Authorization: Bearer <token>` (`401` otherwise).
  `cors_block` advertises only `GET, OPTIONS` — never `POST` to `*`. The helpers are
  `request_origin_ok()` / `request_token_ok()` in `src/net/http.c`.
- **ZT-013 (fixed)** — the `/ws` upgrade and the `/stream` SSE validate `Origin`/`Host` via
  the same helper before streaming device output.

The built-in web UI is same-origin, so it works without a token; set `--http-token` when you
tunnel or proxy the port beyond loopback. The bounded `http_write_all()` (EAGAIN-aware, 2 s
deadline) means a slow webroot transfer no longer truncates or stalls the loop (**ZT-011**).

Also note: `--http` parses its port with `strtol` and range-checks 1–65535 (**ZT-020**,
`src/main.c`).

---

_Last updated: 2026-06-03._
