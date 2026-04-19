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

/* Map F-key index (1..12) from escape sequence. Returns -1 if not an F-key. */
int fkey_index(const unsigned char *buf, size_t n) {
    /* F1..F4 as SS3: \033OP..S */
    if (n >= 3 && buf[0] == 0x1B && buf[1] == 'O') {
        switch (buf[2]) {
        case 'P': return 1;
        case 'Q': return 2;
        case 'R': return 3;
        case 'S': return 4;
        default: return -1;
        }
    }
    /* F5..F12 as CSI: \033[<n>~ */
    if (n >= 4 && buf[0] == 0x1B && buf[1] == '[' && buf[n - 1] == '~') {
        int v = 0;
        for (size_t i = 2; i < n - 1; i++) {
            if (buf[i] < '0' || buf[i] > '9') return -1;
            v = v * 10 + (buf[i] - '0');
        }
        switch (v) {
        case 15: return 5;
        case 17: return 6;
        case 18: return 7;
        case 19: return 8;
        case 20: return 9;
        case 21: return 10;
        case 23: return 11;
        case 24: return 12;
        default: return -1;
        }
    }
    return -1;
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
