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

/* Max wall-clock we keep retrying a blocked serial write before giving up,
 * so a wedged line (hardware/software flow control held off, CTS stuck low)
 * can't freeze the single-threaded UI loop indefinitely (ZT-026). The
 * deadline is reset whenever a write makes progress, so a slow-but-moving
 * link is never penalised — only a genuinely stuck one. */
#define ZT_TX_STALL_DEADLINE_S 3.0

/* ------------------------------ send ------------------------------------- */

/* Apply outbound transforms (EOL mapping, then Telnet IAC escaping) to the
 * caller's buffer. On entry @c *buf and @c *n point at the user data; on
 * return they describe the transformed bytes ready for write(2). Either
 * the original buffer is returned unchanged (no transform configured) or
 * a fresh allocation lives in @c *out_heap (caller frees on completion).
 * Returns false on allocation failure (nothing to send). */
static bool tx_preprocess(zt_ctx *c, const unsigned char **buf, size_t *n,
                          unsigned char **out_heap) {
    *out_heap     = NULL;
    bool need_eol = (c->proto.map_out != ZT_EOL_NONE);
    bool need_tn  = c->serial.telnet;
    if (!need_eol && !need_tn) return true;

    unsigned char *stage1 = NULL; /* after EOL */
    if (need_eol) {
        size_t cap = ZT_EOL_OUT_CAP(*n);
        stage1     = malloc(cap);
        if (!stage1) {
            /* ZT-025: a silent drop here looks like a dead link. Tell the
             * operator the TX was lost to OOM rather than swallowing it. */
            set_flash(c, "TX dropped \xe2\x80\x94 out of memory");
            return false;
        }
        *n =
            eol_translate_out(c->proto.map_out, &c->proto.eol_state_out, *buf, *n, stage1, cap);
        *buf = stage1;
    }

    if (need_tn) {
        size_t         cap2   = (*n) * 2;
        unsigned char *stage2 = malloc(cap2 ? cap2 : 1);
        if (!stage2) {
            set_flash(c, "TX dropped \xe2\x80\x94 out of memory"); /* ZT-025 */
            free(stage1);
            return false;
        }
        *n   = telnet_tx_escape(*buf, *n, stage2, cap2);
        *buf = stage2;
        free(stage1);
        *out_heap = stage2;
    } else {
        *out_heap = stage1;
    }

    if (!*n) {
        free(*out_heap);
        *out_heap = NULL;
        return false;
    }
    return true;
}

void trickle_send(zt_ctx *c, const unsigned char *buf, size_t n) {
    if (c->serial.fd < 0 || !n) return;

    unsigned char *heap = NULL;
    if (!tx_preprocess(c, &buf, &n, &heap)) return;

    log_write_tx(c, buf, n);
    http_broadcast_tx(c, buf, n);
    struct timespec d = {0, ZT_FLUSH_DELAY_US * 1000L};
    struct timespec t_progress;
    now(&t_progress);
    for (size_t i = 0; i < n; i++) {
        for (;;) {
            ssize_t w = write(c->serial.fd, &buf[i], 1);
            if (w == 1) {
                c->core.tx_bytes++;
                now(&t_progress); /* progress resets the stall deadline */
                break;
            }
            if (w < 0 && errno == EINTR) continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                struct pollfd p = {.fd = c->serial.fd, .events = POLLOUT};
                if (poll(&p, 1, 250) < 0 && errno != EINTR) goto out;
                struct timespec tnow;
                now(&tnow);
                if (ts_diff_sec(&tnow, &t_progress) > ZT_TX_STALL_DEADLINE_S) {
                    set_flash(c, "TX stalled (flow control?) \xe2\x80\x94 dropped %zu byte%s",
                              n - i, (n - i) == 1 ? "" : "s");
                    goto out;
                }
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

    unsigned char *heap = NULL;
    if (!tx_preprocess(c, &buf, &n, &heap)) return;

    log_write_tx(c, buf, n);
    http_broadcast_tx(c, buf, n);
    size_t          off = 0;
    struct timespec t_progress;
    now(&t_progress);
    while (off < n) {
        ssize_t w = write(c->serial.fd, buf + off, n - off);
        if (w > 0) {
            c->core.tx_bytes += (uint64_t)w;
            off += (size_t)w;
            now(&t_progress); /* progress resets the stall deadline */
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = {.fd = c->serial.fd, .events = POLLOUT};
            if (poll(&p, 1, 250) < 0 && errno != EINTR) break;
            struct timespec tnow;
            now(&tnow);
            if (ts_diff_sec(&tnow, &t_progress) > ZT_TX_STALL_DEADLINE_S) {
                set_flash(c, "TX stalled (flow control?) \xe2\x80\x94 dropped %zu byte%s",
                          n - off, (n - off) == 1 ? "" : "s");
                break;
            }
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
