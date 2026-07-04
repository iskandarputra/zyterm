/**
 * @file    http.c
 * @brief   HTTP bridge server core: connection table, lifecycle, event pump.
 *
 * Owns the 16-slot connection table (@c g_conn) and the socket lifecycle. A
 * loopback listener is accepted non-blocking; each connection carries its own
 * request buffer and is pumped for headers on the loop tick (never a blocking
 * read — that used to let a slowloris client freeze the UI/serial loop). Once a
 * request is fully buffered it is handed to the router (http_routes.c); dead
 * SSE/WS peers are reaped each tick (ZT-043). The routing, trust checks, crypto
 * and egress live in the sibling http_*.c units (see http_internal.h).
 *
 * Endpoints (routed in http_routes.c):
 *   GET /           — built-in live-RX UI (http_asset.c)
 *   GET /stream     — Server-Sent Events stream of RX bytes
 *   GET /ws         — WebSocket upgrade, RFC 6455, binary frames of RX
 *   GET /metrics    — Prometheus-style snapshot
 *   POST /tx        — write request body to the serial line
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "http_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---- Connection table ----
 *
 * HC_NEW slots are accepted but still parsing the request line + headers. Each
 * connection carries its own @c req_buf and @c accepted_at; http_tick() pumps
 * reads non-blocking and times out idle requests after HC_HEADER_TIMEOUT_S
 * without ever blocking the main loop (slowloris defence). The table layout
 * lives in http_internal.h so the router and egress can read it. */
#define HC_HEADER_TIMEOUT_S 5.0
hc_t g_conn[HC_MAX];

void hc_close(int i) {
    if (g_conn[i].fd >= 0) {
        close(g_conn[i].fd);
        g_conn[i].fd = -1;
    }
}

/* Bounded, EAGAIN-aware full write for one-shot HTTP responses.
 *
 * ZT-011 (INVARIANTS §7): the client fds are SOCK_NONBLOCK, and the loop's
 * zt_write_all() only retries EINTR — using it to push a large --webroot file
 * truncated the response on the first EAGAIN. This waits for writability, but
 * with an overall deadline so a stalled peer can't hang the loop (§3). */
#define HTTP_WRITE_DEADLINE_MS 2000
int http_write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p   = (const unsigned char *)buf;
    size_t               rem = n;
    /* Absolute deadline captured once at entry (ZT-044): the old per-write
     * reset meant a client draining a slow trickle — each read landing inside
     * the 250 ms poll window — kept resetting the timer and held the loop here
     * for the whole (up to ~16.7 KB) response. Bound total wall time regardless
     * of incremental progress so the single-threaded loop stays responsive. */
    struct timespec start;
    now(&start);
    while (rem > 0) {
        ssize_t w = write(fd, p, rem);
        if (w > 0) {
            p += (size_t)w;
            rem -= (size_t)w;
            continue;
        }
        if (w == 0) return -1;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        struct pollfd pf = {.fd = fd, .events = POLLOUT};
        int           pr = poll(&pf, 1, 250);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        struct timespec nowt;
        now(&nowt);
        if (ts_diff_sec(&nowt, &start) * 1000.0 >= HTTP_WRITE_DEADLINE_MS)
            return -1; /* stalled */
    }
    return 0;
}

/* Non-blocking "write every byte or fail" for streaming frames. A partial
 * write corrupts SSE/WS framing, so the peer must be dropped — never blocks
 * (broadcast runs on the loop tick). */
int http_stream_write(int fd, const void *buf, size_t n) {
    const unsigned char *p   = (const unsigned char *)buf;
    size_t               rem = n;
    while (rem > 0) {
        ssize_t w = write(fd, p, rem);
        if (w > 0) {
            p += (size_t)w;
            rem -= (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        return -1; /* EAGAIN or hard error → caller closes the peer */
    }
    return 0;
}

/* ---- Server setup ---- */
int http_start(zt_ctx *c, int port) {
    if (!c || c->net.http_fd >= 0) return -1;
    for (int i = 0; i < HC_MAX; i++)
        g_conn[i].fd = -1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family         = AF_INET;
    a.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
    a.sin_port           = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    c->net.http_fd   = fd;
    c->net.http_port = port;
    log_notice(c, "http bridge on http://127.0.0.1:%d/", port);
    return 0;
}

void http_stop(zt_ctx *c) {
    if (!c) return;
    for (int i = 0; i < HC_MAX; i++)
        hc_close(i);
    if (c->net.http_fd >= 0) {
        close(c->net.http_fd);
        c->net.http_fd = -1;
    }
}

/* Stash a freshly-accepted fd into an HC_NEW slot. The actual request
 * read + classify happens in hc_pump_new() on subsequent http_tick()
 * passes — never inside a blocking loop. */
static int accept_one(zt_ctx *c, int lfd) {
    (void)c;
    int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (cfd < 0) return -1;
    for (int i = 0; i < HC_MAX; i++) {
        if (g_conn[i].fd < 0) {
            g_conn[i].fd      = cfd;
            g_conn[i].type    = HC_NEW;
            g_conn[i].req_len = 0;
            now(&g_conn[i].accepted_at);
            return 0;
        }
    }
    /* No free slot: drop politely so the kernel TCP backlog can drain. */
    close(cfd);
    return 0;
}

/* Non-blocking header-read pump for an HC_NEW slot. Reads what's
 * available; on "\r\n\r\n" classifies and either upgrades the slot or
 * closes it. Idle slots past HC_HEADER_TIMEOUT_S are dropped — that's
 * the slowloris defence. */
static void hc_pump_new(zt_ctx *c, int i) {
    hc_t *h = &g_conn[i];
    for (;;) {
        if (h->req_len + 1 >= sizeof h->req_buf) {
            /* Headers oversized — refuse. */
            send_text_c(c, h->fd, "431 Request Header Fields Too Large", "text/plain", "", 0);
            hc_close(i);
            return;
        }
        ssize_t r = read(h->fd, h->req_buf + h->req_len, sizeof h->req_buf - 1 - h->req_len);
        if (r > 0) {
            h->req_len += (size_t)r;
            h->req_buf[h->req_len] = '\0';
            char *hdrend           = memmem(h->req_buf, h->req_len, "\r\n\r\n", 4);
            if (hdrend) {
                size_t header_len = (size_t)(hdrend - h->req_buf) + 4;
                /* ZT-034: for POST, don't dispatch until the whole declared
                 * body has arrived — otherwise a body split across TCP
                 * segments is truncated/dropped while we return 204. */
                if (h->req_len >= 5 && strncmp(h->req_buf, "POST ", 5) == 0) {
                    long clen = http_content_length(h->req_buf, header_len);
                    if (clen > (long)(sizeof h->req_buf - 1 - header_len)) {
                        /* declared body can't fit our bounded request buffer */
                        send_text_c(c, h->fd, "413 Payload Too Large", "text/plain", "", 0);
                        hc_close(i);
                        return;
                    }
                    if (clen > 0 && h->req_len - header_len < (size_t)clen)
                        continue; /* wait for the rest of the body */
                }
                classify_request(c, i);
                return;
            }
            continue;
        }
        if (r == 0) {
            hc_close(i);
            return;
        } /* peer closed */
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        hc_close(i);
        return;
    }
    /* Drain timeout: don't let a stalled client tie up the slot. */
    struct timespec t;
    now(&t);
    if (ts_diff_sec(&t, &h->accepted_at) > HC_HEADER_TIMEOUT_S) {
        hc_close(i);
    }
}

void http_tick(zt_ctx *c) {
    if (!c || c->net.http_fd < 0) return;
    /* Drain new accepts (non-blocking). */
    while (accept_one(c, c->net.http_fd) == 0) {}
    /* Pump any slots still mid-request. */
    for (int i = 0; i < HC_MAX; i++) {
        if (g_conn[i].fd >= 0 && g_conn[i].type == HC_NEW) hc_pump_new(c, i);
    }
    /* ZT-043: reap established SSE/WS peers that have hung up. Otherwise a dead
     * peer's slot is freed only by a failing broadcast write, so on an idle
     * link (no RX/TX/input) 16 half-open clients exhaust HC_MAX and the bridge
     * stops accepting. Poll each stream fd non-blocking for hangup / readable
     * EOF and drop it. */
    for (int i = 0; i < HC_MAX; i++) {
        if (g_conn[i].fd < 0 || g_conn[i].type == HC_NEW) continue;
        struct pollfd pf = {.fd = g_conn[i].fd, .events = POLLIN};
        if (poll(&pf, 1, 0) <= 0) continue;
        if (pf.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            hc_close(i);
            continue;
        }
        if (pf.revents & POLLIN) {
            char    b[64];
            ssize_t r = recv(g_conn[i].fd, b, sizeof b, MSG_DONTWAIT);
            if (r == 0)
                hc_close(i); /* peer sent FIN (e.g. browser tab closed) */
            else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                hc_close(i);
            /* r > 0: unsolicited client bytes on a server→client stream — drop
             * them and keep the peer. */
        }
    }
}
