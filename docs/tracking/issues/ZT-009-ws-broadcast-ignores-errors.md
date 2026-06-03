# ZT-009: WebSocket broadcast ignores write errors — dead peers leak, partial frames corrupt the stream

- **Severity:** 🟠 medium (unlike the SSE path, the WS path never closes a
  broken peer; short/failed frame writes corrupt the WS bytestream and the slot
  is leaked, feeding the WS-slot-exhaustion DoS, ZT-017)
- **Area:** net (HTTP bridge — WebSocket)
- **Status:** open  (recorded 2026-06-03; not yet fixed)
- **Location:** `src/net/http.c:1037`

## Root cause

In `http_broadcast`, the SSE branch inspects the `write()` result and tears down
a peer that errors or short-writes:

```c
if (g_conn[i].type == HC_SSE) {
    ssize_t w = write(g_conn[i].fd, ev, en);
    if (w < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) hc_close(i);
    } else if ((size_t)w != en)
        hc_close(i);
}
```
(`src/net/http.c:1027`)

The WebSocket branch does none of this — it just fires the frame and discards
whatever happens:

```c
} else if (g_conn[i].type == HC_WS) {
    ws_frame_text(g_conn[i].fd, buf, chunk);   /* return value? there is none */
}
```
(`src/net/http.c:1036`)

`ws_frame_text` itself is `void` and explicitly throws away both writes:

```c
static void ws_frame_text(int fd, const unsigned char *buf, size_t n) {
    ...
    (void)zt_write_all(fd, hdr, hl);   /* header — result discarded */
    (void)zt_write_all(fd, buf, n);    /* payload — result discarded */
}
```
(`src/net/http.c:995`)

`zt_write_all` does report failure — it returns `-1` on any non-`EINTR` write
error (`src/core/core.c:187`) — but both call sites cast it to `(void)`. Two
distinct failure modes follow:

1. **Dead peers are never reaped.** When a WebSocket client disconnects, the
   socket write fails (EPIPE / ECONNRESET). The SSE path would `hc_close()` the
   slot; the WS path ignores the error, so the `HC_WS` slot stays occupied
   forever. With only `HC_MAX == 16` slots (`src/net/http.c:159`), 16 ungraceful
   WS disconnects exhaust the table and the bridge stops accepting new
   connections — this is the same slot leak escalated to a DoS in ZT-017.

2. **Partial frames corrupt the stream.** `ws_frame_text` writes the frame
   header and payload as two separate `zt_write_all` calls. If the header write
   succeeds but the payload write fails partway (peer slow, kernel buffer full,
   connection dropping), the receiver has consumed a header announcing `n` bytes
   but receives fewer — every subsequent frame on that connection is then
   misframed. Because the error is swallowed, the connection is left in this
   desynchronized state instead of being closed. (`zt_write_all` also retries
   only `EINTR`; on a non-blocking fd a partial write under `EAGAIN` returns
   `-1`, which is likewise ignored here.)

## Trigger / repro

1. Start the bridge: `zyterm /dev/ttyUSB0 --http 8080`.
2. Connect a WebSocket client to the RX stream, then kill it ungracefully
   (drop the TCP connection without a close handshake).
3. Drive device RX so `http_broadcast` writes to the now-dead WS fd.
4. Observe: no `hc_close()` runs for that slot; it remains `HC_WS` in `g_conn`.
   Repeat 16 times and the bridge can no longer accept connections (ties into
   ZT-017).

## Fix direction

Make the WS branch error-aware, mirroring SSE:

- Give `ws_frame_text` an `int` return that propagates the `zt_write_all`
  results, or check the writes inline in the broadcast loop.
- In `http_broadcast` (and `http_broadcast_tx`), `hc_close(i)` the WS slot on any
  non-`EAGAIN` write error, exactly as the SSE branch does.
- Treat a short/failed payload write after a successful header write as fatal for
  that connection — close it rather than leave a desynchronized stream.

This is the corrective half of the WS-slot leak (ZT-017) and should be fixed
together with the 4 KiB truncation (ZT-007), since all three live in
`http_broadcast`.

See theme (E) "non-blocking fd + blocking write helper" (ZT-009/011/017) in
[`../../plans/RELIABILITY_HARDENING.md`](../../plans/RELIABILITY_HARDENING.md),
the WebSocket framing notes in
[`../../design/HTTP_BRIDGE.md`](../../design/HTTP_BRIDGE.md), the network/IPC
trust-boundary rules in
[`../../invariants/INVARIANTS.md`](../../invariants/INVARIANTS.md) §7, and the
board row in [`../KNOWN_ISSUES.md`](../KNOWN_ISSUES.md).

## Verify

- Integration: connect a WS client, drop it ungracefully, broadcast, and assert
  the `HC_WS` slot is closed (`g_conn[i].fd == -1`); repeat past `HC_MAX` and
  confirm new connections still succeed.
- Integration: stall a WS peer mid-payload and assert the connection is closed
  rather than left producing misframed subsequent messages.
- Manual: run the repro and watch the slot count recover after disconnects.
