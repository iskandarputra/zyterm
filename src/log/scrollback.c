/**
 * @file    scrollback.c
 * @brief   Scrollback ring-buffer storage (the line ring itself).
 *
 * The viewport — drawing, scrolling, and mouse text-selection/copy — lives in
 * tui/scrollback_view.c (it reaches up into render/proto, so it belongs in the
 * tui layer, not here). This file is pure storage: append a line, free the ring.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */

#include "zt_ctx.h"
#include "zyterm/internal/log.h"

#include <ctype.h>
#include <malloc.h> /* malloc_usable_size — grow-only slot reuse */
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

/* ------------------------------ scrollback storage ----------------------- */

void scrollback_push(zt_ctx *c) {
    if (!c->log.sb_lines || !c->log.line_len) return;
    size_t need = c->log.line_len + 1;

    /* Pick the slot this line lands in — append while filling, else recycle the
     * oldest — but don't commit the ring pointers until the copy succeeds, so an
     * allocation failure leaves the ring exactly as it was. */
    bool full = c->log.sb_count >= ZT_SCROLLBACK_CAP;
    int  slot = full ? c->log.sb_head : (c->log.sb_head + c->log.sb_count) % ZT_SCROLLBACK_CAP;

    /* Reuse the slot's existing allocation when it is already large enough. The
     * old code malloc()'d a fresh buffer for every line and free()'d it on
     * eviction — a churn of ~one malloc+free per RX line forever. Once the ring
     * has filled, each slot has reached its high-water length, so reuse makes
     * the steady state allocation-free; a longer line grows the slot in place.
     * Readers still see a NUL-terminated char* (the contract is unchanged). */
    char *s = c->log.sb_lines[slot];
    if (!s || malloc_usable_size(s) < need) {
        char *ns = realloc(s, need);
        if (!ns) return; /* OOM: drop this line, ring intact */
        s = ns;
    }
    memcpy(s, c->log.line, c->log.line_len);
    s[c->log.line_len]    = '\0';
    c->log.sb_lines[slot] = s;

    if (full)
        c->log.sb_head = (c->log.sb_head + 1) % ZT_SCROLLBACK_CAP;
    else
        c->log.sb_count++;
}

void scrollback_free(zt_ctx *c) {
    if (!c->log.sb_lines) return;
    for (int i = 0; i < ZT_SCROLLBACK_CAP; i++)
        free(c->log.sb_lines[i]);
    free(c->log.sb_lines);
    c->log.sb_lines = NULL;
}
