/**
 * @file fuzz_http.c
 * @brief libFuzzer target for the HTTP request parser / router (net/http_routes.c).
 *
 * classify_request() is the bridge's trust boundary: it parses the request line
 * (method + path), dispatches on the exact path, validates Origin/Host + bearer
 * token on the sensitive routes, parses Content-Length, extracts the
 * Sec-WebSocket-Key, and forwards POST bodies to the serial line. All of it runs
 * on attacker-controlled bytes off a loopback socket. This drives it with the
 * fuzz input placed directly in a connection slot's request buffer (the state
 * hc_pump_new() would have accumulated), so the parser/router is exercised in
 * isolation without a live listener.
 *
 * The slot's fd is one end of a socketpair (non-blocking, so response writes
 * never block the fuzzer); tx_direct is a no-op sink so a POST /tx body is
 * "sent" without touching a real device. classify_request either closes the
 * slot or promotes it to SSE/WS; either way we close both ends per run.
 *
 * Build under -fsanitize=fuzzer,address,undefined (see the `fuzz` CI job /
 * `make fuzz`, which auto-discovers every tests/fuzz/fuzz_*.c). A crash/leak/UB
 * fails CI.
 *
 * @author  Iskandar Putra
 * @license MIT
 */
#include "zt_ctx.h"
#include "net/http_internal.h"

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void noop_tx(zt_ctx *c, const unsigned char *b, size_t n) {
    (void)c;
    (void)b;
    (void)n;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 0;
    /* Non-blocking server side: the router's response writes must never block. */
    int fl = fcntl(sv[0], F_GETFL, 0);
    fcntl(sv[0], F_SETFL, fl | O_NONBLOCK);

    zt_ctx c;
    memset(&c, 0, sizeof c);
    c.serial.fd      = sv[1]; /* >= 0 so the POST /tx body path is reachable */
    c.log.fd         = -1;
    c.net.http_fd    = -1;
    c.core.tx_direct = noop_tx; /* discard any forwarded POST body */

    for (int i = 0; i < HC_MAX; i++)
        g_conn[i].fd = -1;
    g_conn[0].fd   = sv[0];
    g_conn[0].type = HC_NEW;
    size_t n       = size < HC_REQ_CAP - 1 ? size : (size_t)(HC_REQ_CAP - 1);
    memcpy(g_conn[0].req_buf, data, n);
    g_conn[0].req_buf[n] = '\0';
    g_conn[0].req_len    = n;

    classify_request(&c, 0);

    if (g_conn[0].fd >= 0) {
        close(g_conn[0].fd);
        g_conn[0].fd = -1;
    }
    close(sv[1]);
    return 0;
}
