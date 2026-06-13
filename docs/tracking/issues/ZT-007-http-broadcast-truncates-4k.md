# ZT-007: `http_broadcast` truncates RX bursts larger than 4 KiB

- **Severity:** 🟠 medium (the web/SSE/WS view silently drops data — it diverges
  from the terminal and the log on any burst >4096 B, with no error surfaced)
- **Area:** net (HTTP bridge)
- **Status:** **fixed** 2026-06-13 on `fix/zt-001-ownership-and-ui-hangs` (recorded 2026-06-03) — see [KNOWN_ISSUES Resolved](../KNOWN_ISSUES.md#resolved)
- **Location:** `src/net/http.c:1020`

## Root cause

`rx_ingest()` hands the HTTP bridge the *entire* RX buffer it received from the
device:

```c
if (c->net.http_fd >= 0) http_broadcast(c, buf, n);
```
(`src/render/render.c:347`)

`n` here is whatever the read produced — up to `ZT_READ_CHUNK`, which is larger
than 4 KiB. But `http_broadcast` only ever encodes and emits the *first* 4096
bytes and then returns:

```c
void http_broadcast(zt_ctx *c, const unsigned char *buf, size_t n) {
    if (!c || !buf || n == 0) return;
    char   b64[8192];
    size_t chunk = n > 4096 ? 4096 : n;   /* clamp, no loop */
    b64enc(buf, chunk, b64);
    ...
    for (int i = 0; i < HC_MAX; i++) {
        ...
        if (g_conn[i].type == HC_SSE) { write(... ev, en ...); }
        else if (g_conn[i].type == HC_WS) { ws_frame_text(g_conn[i].fd, buf, chunk); }
    }
}
```
(`src/net/http.c:1017`)

The `chunk = n > 4096 ? 4096 : n` clamp exists to bound the on-stack base64
buffer (`b64[8192]` holds the encoding of 4096 bytes plus the SSE framing in
`ev[9000]`). But there is **no loop** over the remaining `n - chunk` bytes —
they are simply discarded. Both delivery branches (SSE at `:1027` and WS at
`:1036`) consume only the single clamped `chunk`.

Result: any single device read of more than 4096 bytes — common at high baud
rates, on bulk dumps, on `cat /dev/...`-style output — appears complete in the
terminal and in the on-disk log, but the HTTP/SSE/WebSocket consumers receive
only the leading 4 KiB. The web view silently desynchronizes from ground truth
with no diagnostic.

## Trigger / repro

1. Start the bridge: `zyterm /dev/ttyUSB0 --http 8080` and open the web UI
   (or `curl -N http://localhost:8080/events`).
2. Have the device emit a single burst larger than 4 KiB (e.g. dump a file, or
   run a peer that writes 16 KiB in one go).
3. Compare the bytes shown in the terminal/log against what the SSE/WS stream
   delivered.
4. Observe: the web side stops at the first 4096 bytes of the burst; the rest
   never arrives.

## Fix direction

Loop over the input in ≤4096-byte segments, encoding and broadcasting each:

```c
for (size_t off = 0; off < n; off += 4096) {
    size_t chunk = (n - off) > 4096 ? 4096 : (n - off);
    /* b64enc(buf + off, chunk, b64); emit to every SSE/WS slot */
}
```

This keeps the bounded `b64[8192]` / `ev[9000]` stack buffers while delivering
the whole burst. Apply the same loop to `http_broadcast_tx`
(`src/net/http.c:1042`), which has the identical clamp. Note this fix is
coupled with ZT-009 (the WS branch must also check write results) — both touch
the same broadcast routine.

See theme (E) "non-blocking fd + write helper" (ZT-009/011/017) in
[`../../plans/RELIABILITY_HARDENING.md`](../../plans/RELIABILITY_HARDENING.md),
the bridge design in
[`../../design/HTTP_BRIDGE.md`](../../design/HTTP_BRIDGE.md), and the board row
in [`../KNOWN_ISSUES.md`](../KNOWN_ISSUES.md).

## Verify

- Integration: connect an SSE and a WS client, feed a single >4 KiB RX burst,
  and assert the concatenated decoded payload received over the stream equals
  the bytes ingested (byte-for-byte, including the tail past 4096).
- Manual: run the repro and confirm the web view matches the terminal on a large
  dump.
