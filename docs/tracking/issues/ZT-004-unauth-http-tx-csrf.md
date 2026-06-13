# ZT-004: unauthenticated HTTP `POST /tx` writes the serial line (CSRF / DNS-rebind RCE)

- **Severity:** 🔴 high (any web page the operator visits can send arbitrary bytes to the device →
  remote command execution on the target)
- **Area:** net (HTTP bridge) / security
- **Status:** **fixed** 2026-06-13 on `fix/zt-001-ownership-and-ui-hangs` (recorded 2026-06-03) — see [KNOWN_ISSUES Resolved](../KNOWN_ISSUES.md#resolved)
- **Location:** `src/net/http.c:907` (`POST /api/send` / `POST /tx` in `classify_request`),
  `src/loop/send.c:109` (`direct_send`)

## Root cause

The HTTP bridge exposes a **state-changing, unauthenticated** endpoint that writes straight to the
serial device, with no caller-identity check and a permissive CORS policy.

`classify_request()` routes both `POST /api/send` and `POST /tx` to the device with no auth and no
Origin/Host validation:

```c
if ((rn >= 14 && strncmp(req, "POST /api/send", 14) == 0) ||
    (rn >= 8  && strncmp(req, "POST /tx",       8)  == 0)) {   /* src/net/http.c:907 */
    const char *body = strstr(req, "\r\n\r\n");
    if (body && c->serial.fd >= 0) {
        body += 4;
        size_t blen = rn - (size_t)(body - req);
        direct_send(c, (const unsigned char *)body, blen);     /* body → serial, unfiltered */
    }
    send_text_c(c, cfd, "204 No Content", "text/plain", "", 0);
    ...
```

There is no `Authorization`/bearer check, no token, and no `Origin`/`Host` comparison anywhere in
`classify_request` (`src/net/http.c:838`) — the only header inspected is `Sec-WebSocket-Key` for the
`/ws` upgrade. `direct_send()` then writes the body bytes directly to the device:

```c
void direct_send(zt_ctx *c, const unsigned char *buf, size_t n) {
    if (c->serial.fd < 0 || !n) return;
    ...
    ssize_t w = write(c->serial.fd, buf + off, n - off);   /* src/loop/send.c:119 */
```

Two amplifiers make this remotely reachable from a browser:

- **CORS wildcard on a state-changing route.** `cors_block()` (`src/net/http.c:604`) emits
  `Access-Control-Allow-Origin: *` with `Access-Control-Allow-Methods: GET, POST, OPTIONS` when
  `--http-cors` is set, inviting cross-origin POSTs. Even without `--http-cors`, a `POST` with a
  `text/plain` body is a **CORS "simple request"** — the browser sends it cross-origin without a
  preflight, and the server acts on it regardless of whether the response is readable.
- **DNS rebinding.** Because there is no `Host`/`Origin` allowlist, an attacker page can rebind its
  hostname to `127.0.0.1` and POST to the local bridge as a same-origin request, bypassing the
  browser's origin checks entirely.

Net effect: any web page the operator visits while `--http <port>` is running can push arbitrary
bytes onto the serial line — i.e. issue device commands (bootloader writes, shell on the target,
config changes). The bridge's local-IPC trust boundary (`INVARIANTS §7`) is violated; this is the
HTTP instance of cross-cutting theme B (unauthenticated local IPC).

## Trigger / repro

1. Operator runs `zyterm --http 8080 /dev/ttyUSB0` (optionally `--http-cors`).
2. Operator visits an attacker-controlled page, which runs:
   ```js
   fetch('http://127.0.0.1:8080/tx', {
     method: 'POST',
     headers: { 'Content-Type': 'text/plain' },  // simple request, no preflight
     body: 'reboot\r'
   });
   ```
3. zyterm reaches `src/net/http.c:907` → `direct_send` → `write(serial.fd, "reboot\r")`. The device
   receives the command. DNS rebinding achieves the same without `--http-cors` and without same-site.

## Fix direction

Treat the HTTP bridge as an authenticated, origin-pinned control channel:

- **Authenticate state-changing routes.** Require a bearer token (`Authorization: Bearer …`, value
  from a CLI flag / env / generated-and-printed secret) on `POST /tx` and `POST /api/send`; reject
  with `401` otherwise. A token in a custom header also forces a CORS preflight, closing the
  simple-request hole.
- **Validate `Origin` and `Host`.** Allowlist expected origins/hosts (default: none / loopback only)
  and reject mismatches — this is the DNS-rebind defense. Apply the same Origin check to the WS
  upgrade (ZT-013) and SSE stream.
- **Never send `Access-Control-Allow-Origin: *` on state-changing routes.** Restrict `cors_block`
  (`src/net/http.c:604`) so write methods are not granted to the wildcard origin; keep CORS for
  read-only GETs only if needed.
- Encode the rule in `INVARIANTS §7`: the network bridge is an untrusted boundary; no route may mutate
  device/serial state without authentication and origin validation.

## Verify

- After the fix, the `fetch` repro above must return `401` and produce **no** bytes on the serial
  line; confirm with a loopback `socat` pty and a TX byte counter.
- Test that a request with a valid bearer token *and* an allowlisted Origin succeeds, while wrong
  token, missing token, or a foreign/rebound `Host`/`Origin` is rejected.
- Confirm `--http-cors` no longer advertises `POST` to `*`, and that a cross-origin simple `POST`
  without the token is refused. Re-run the HTTP integration suite.
