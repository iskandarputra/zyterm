/**
 * @file macros.c
 * @brief F1..F12 macro storage and firing
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

/* ------------------------------ macros ----------------------------------- */

/* Parse an F-key escape sequence at the start of @p buf. Returns the
 * 1..12 F-key index and the number of consumed bytes via @p out_consumed
 * (or 0 if not an F-key). Returns -1 if not recognised; callers can then
 * dispatch the remainder (after @p *out_consumed) elsewhere. */
int fkey_index_consume(const unsigned char *buf, size_t n, size_t *out_consumed) {
    if (out_consumed) *out_consumed = 0;
    /* F1..F4 as SS3: \033OP..S */
    if (n >= 3 && buf[0] == 0x1B && buf[1] == 'O') {
        int idx = -1;
        switch (buf[2]) {
        case 'P': idx = 1; break;
        case 'Q': idx = 2; break;
        case 'R': idx = 3; break;
        case 'S': idx = 4; break;
        default: return -1;
        }
        if (out_consumed) *out_consumed = 3;
        return idx;
    }
    /* F5..F12 as CSI: \033[<digits>~ */
    if (n >= 4 && buf[0] == 0x1B && buf[1] == '[') {
        size_t i = 2;
        int    v = 0;
        while (i < n && buf[i] >= '0' && buf[i] <= '9') {
            v = v * 10 + (buf[i] - '0');
            i++;
        }
        if (i == 2 || i >= n || buf[i] != '~') return -1;
        size_t consumed = i + 1;
        int    idx;
        switch (v) {
        case 15: idx = 5; break;
        case 17: idx = 6; break;
        case 18: idx = 7; break;
        case 19: idx = 8; break;
        case 20: idx = 9; break;
        case 21: idx = 10; break;
        case 23: idx = 11; break;
        case 24: idx = 12; break;
        default: return -1;
        }
        if (out_consumed) *out_consumed = consumed;
        return idx;
    }
    return -1;
}

/* Back-compat wrapper: returns the F-key index ignoring trailing bytes.
 * Prefer @c fkey_index_consume so the caller can dispatch the remainder. */
int fkey_index(const unsigned char *buf, size_t n) {
    return fkey_index_consume(buf, n, NULL);
}

/* Expand \r \n \t \\ \xNN escapes in-place in dst. */
size_t expand_escapes(const char *src, char *dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < cap; i++) {
        if (src[i] == '\\' && src[i + 1]) {
            char e = src[++i];
            switch (e) {
            case 'r': dst[o++] = '\r'; break;
            case 'n': dst[o++] = '\n'; break;
            case 't': dst[o++] = '\t'; break;
            case '\\': dst[o++] = '\\'; break;
            case 'x': {
                if (src[i + 1] && src[i + 2]) {
                    char  hx[3] = {src[i + 1], src[i + 2], 0};
                    char *e2    = NULL;
                    long  v     = strtol(hx, &e2, 16);
                    if (e2 == hx + 2) {
                        dst[o++] = (char)v;
                        i += 2;
                    } else
                        dst[o++] = e;
                } else
                    dst[o++] = e;
                break;
            }
            default: dst[o++] = e; break;
            }
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = 0;
    return o;
}

/* ------------------------------ send ------------------------------------- */

void trickle_send(zt_ctx *c, const unsigned char *buf, size_t n);
void direct_send(zt_ctx *c, const unsigned char *buf, size_t n);

void macro_fire(zt_ctx *c, int fkey_idx) {
    if (fkey_idx < 1 || fkey_idx > ZT_MACRO_COUNT) return;
    const char *m = c->ext.macros[fkey_idx - 1];
    if (!m || !*m) return;
    char   exp[1024];
    size_t el = expand_escapes(m, exp, sizeof exp);
    if (el) trickle_send(c, (const unsigned char *)exp, el);
}
