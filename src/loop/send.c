/**
 * @file send.c
 * @brief Outbound writes: trickle / direct / flush_unsent
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------ send ------------------------------------- */

void trickle_send(zt_ctx *c, const unsigned char *buf, size_t n) {
    if (c->serial.fd < 0 || !n) return;

    unsigned char  scratch[1024];
    unsigned char *heap = NULL;
    if (c->proto.map_out != ZT_EOL_NONE) {
        size_t         cap = ZT_EOL_OUT_CAP(n);
        unsigned char *xb  = (cap <= sizeof scratch) ? scratch
                                                     : (heap = malloc(cap));
        if (!xb) return;
        n   = eol_translate_out(c->proto.map_out, &c->proto.eol_state_out,
                                buf, n, xb, cap);
        buf = xb;
        if (!n) { free(heap); return; }
    }

    log_write_tx(c, buf, n);
    http_broadcast_tx(c, buf, n);
    struct timespec d = {0, ZT_FLUSH_DELAY_US * 1000L};
    for (size_t i = 0; i < n; i++) {
        for (;;) {
            ssize_t w = write(c->serial.fd, &buf[i], 1);
            if (w == 1) {
                c->core.tx_bytes++;
                break;
            }
            if (w < 0 && errno == EINTR) continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                struct pollfd p = {.fd = c->serial.fd, .events = POLLOUT};
                if (poll(&p, 1, 250) < 0 && errno != EINTR) goto out;
                continue;
            }
            goto out;
        }
        if (i + 1 < n) nanosleep(&d, NULL);
    }
out:
    free(heap);
}

void direct_send(zt_ctx *c, const unsigned char *buf, size_t n) {
    if (c->serial.fd < 0 || !n) return;

    unsigned char  scratch[1024];
    unsigned char *heap = NULL;
    if (c->proto.map_out != ZT_EOL_NONE) {
        size_t         cap = ZT_EOL_OUT_CAP(n);
        unsigned char *xb  = (cap <= sizeof scratch) ? scratch
                                                     : (heap = malloc(cap));
        if (!xb) return;
        n   = eol_translate_out(c->proto.map_out, &c->proto.eol_state_out,
                                buf, n, xb, cap);
        buf = xb;
        if (!n) { free(heap); return; }
    }

    log_write_tx(c, buf, n);
    http_broadcast_tx(c, buf, n);
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(c->serial.fd, buf + off, n - off);
        if (w > 0) {
            c->core.tx_bytes += (uint64_t)w;
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = {.fd = c->serial.fd, .events = POLLOUT};
            if (poll(&p, 1, 250) < 0 && errno != EINTR) break;
            continue;
        }
        break;
    }
    free(heap);
}

void flush_unsent(zt_ctx *c) {
    if (c->tui.sent_len < c->tui.input_len) {
        trickle_send(c, c->tui.input_buf + c->tui.sent_len, c->tui.input_len - c->tui.sent_len);
        c->tui.sent_len = c->tui.input_len;
        c->tui.cursor   = 0;
    }
}
