/**
 * @file    http_stream.c
 * @brief   Server→client egress for the HTTP bridge: SSE + WebSocket.
 *
 * Once a connection is promoted to HC_SSE or HC_WS (by the router), RX/TX and
 * input-echo bytes are pushed to it here. SSE frames are base64 `data:` events;
 * WS frames are binary (opcode 0x2 — device RX is arbitrary 8-bit data, so a
 * TEXT frame with a non-UTF-8 byte would make the client close 1007, ZT-035).
 * Both paths segment large bursts (ZT-007) and reap dead peers on a failing
 * write (ZT-009/ZT-017). Broadcasts short-circuit when no stream peer is open
 * so an idle --http bridge pays no encode cost.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "http_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

/* Send one WS binary frame. Returns 0 on success, -1 if the peer's socket
 * can't take the whole frame (ZT-009): the caller must then close it, because
 * a partial frame desyncs the stream and a never-reaped dead peer exhausts the
 * 16 connection slots (ZT-017). Binary (opcode 0x2), not text: device RX is
 * arbitrary 8-bit data, and a TEXT frame carrying a non-UTF-8 byte makes every
 * conformant client fail the connection with close code 1007 (ZT-035). */
static int ws_frame_binary(int fd, const unsigned char *buf, size_t n) {
    unsigned char hdr[10];
    size_t        hl = 0;
    hdr[0]           = 0x82; /* FIN + binary */
    if (n < 126) {
        hdr[1] = (unsigned char)n;
        hl     = 2;
    } else if (n < 65536) {
        hdr[1] = 126;
        hdr[2] = (unsigned char)((n >> 8) & 0xFF);
        hdr[3] = (unsigned char)(n & 0xFF);
        hl     = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++)
            hdr[2 + i] = (unsigned char)((n >> (56 - i * 8)) & 0xFF);
        hl = 10;
    }
    if (http_stream_write(fd, hdr, hl) != 0) return -1;
    if (http_stream_write(fd, buf, n) != 0) return -1;
    return 0;
}

/* Any SSE/WS peer actually connected? A 16-slot scan is far cheaper than
 * base64-encoding the whole RX stream at line rate for nobody — merely leaving
 * the --http bridge on with no open tab used to pay full encode cost. */
static bool http_has_stream_peer(void) {
    for (int i = 0; i < HC_MAX; i++)
        if (g_conn[i].fd >= 0 && (g_conn[i].type == HC_SSE || g_conn[i].type == HC_WS))
            return true;
    return false;
}

void http_broadcast(zt_ctx *c, const unsigned char *buf, size_t n) {
    if (!c || !buf || n == 0 || !http_has_stream_peer()) return;
    /* ZT-007: iterate the whole payload in <=4096-byte segments. The old code
     * encoded only the first 4096 bytes once, silently dropping the rest of any
     * RX burst from both the SSE and WS views. */
    for (size_t off = 0; off < n; off += 4096) {
        size_t               seglen = n - off > 4096 ? 4096 : n - off;
        const unsigned char *seg    = buf + off;
        char                 b64[8192];
        zt_base64(seg, seglen, b64);
        char   ev[9000];
        int    en_raw = snprintf(ev, sizeof ev, "data: %s\n\n", b64);
        size_t en     = snprintf_len(en_raw, sizeof ev);
        for (int i = 0; i < HC_MAX; i++) {
            if (g_conn[i].fd < 0) continue;
            if (g_conn[i].type == HC_SSE) {
                ssize_t w = write(g_conn[i].fd, ev, en);
                /* EAGAIN: kernel buffer full — drop this event rather than
                 * block. Any other error, or a short write (can't queue the
                 * remainder here), means the peer is gone or stalled — close. */
                if (w < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) hc_close(i);
                } else if ((size_t)w != en)
                    hc_close(i);
            } else if (g_conn[i].type == HC_WS) {
                /* ZT-009/ZT-017: check the frame write and reap dead peers. */
                if (ws_frame_binary(g_conn[i].fd, seg, seglen) != 0) hc_close(i);
            }
        }
    }
}

void http_broadcast_tx(zt_ctx *c, const unsigned char *buf, size_t n) {
    if (!c || !buf || n == 0 || c->net.http_fd < 0 || !http_has_stream_peer()) return;
    /* ZT-007: segment the payload like http_broadcast() rather than capping the
     * TX echo at one 4096-byte chunk. */
    for (size_t off = 0; off < n; off += 4096) {
        size_t seglen = n - off > 4096 ? 4096 : n - off;
        char   b64[8192];
        zt_base64(buf + off, seglen, b64);
        char   ev[9000];
        int    en_raw = snprintf(ev, sizeof ev, "event: tx\ndata: %s\n\n", b64);
        size_t en     = snprintf_len(en_raw, sizeof ev);
        for (int i = 0; i < HC_MAX; i++) {
            if (g_conn[i].fd < 0) continue;
            if (g_conn[i].type == HC_SSE) {
                ssize_t w = write(g_conn[i].fd, ev, en);
                if (w < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) hc_close(i);
                } else if ((size_t)w != en)
                    hc_close(i);
            }
        }
    }
}

void http_notify_input(zt_ctx *c) {
    if (!c || c->net.http_fd < 0) return;
    /* Send the unsent portion of the input buffer (what's visible in the bar). */
    size_t unsent = c->tui.input_len > c->tui.sent_len ? c->tui.input_len - c->tui.sent_len : 0;
    char   b64[ZT_INPUT_CAP * 2];
    if (unsent > 0)
        zt_base64(c->tui.input_buf + c->tui.sent_len, unsent, b64);
    else
        b64[0] = '\0';
    char   ev[ZT_INPUT_CAP * 2 + 64];
    int    en_raw = snprintf(ev, sizeof ev, "event: input\ndata: %s\n\n", b64);
    size_t en     = snprintf_len(en_raw, sizeof ev);
    for (int i = 0; i < HC_MAX; i++) {
        if (g_conn[i].fd < 0) continue;
        if (g_conn[i].type == HC_SSE) {
            ssize_t w = write(g_conn[i].fd, ev, en);
            if (w < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) hc_close(i);
            } else if ((size_t)w != en)
                hc_close(i);
        }
    }
}
