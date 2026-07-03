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
    char *s = malloc(c->log.line_len + 1);
    if (!s) return;
    memcpy(s, c->log.line, c->log.line_len);
    s[c->log.line_len] = '\0';

    int slot;
    if (c->log.sb_count < ZT_SCROLLBACK_CAP) {
        slot = (c->log.sb_head + c->log.sb_count) % ZT_SCROLLBACK_CAP;
        c->log.sb_count++;
    } else {
        slot = c->log.sb_head;
        free(c->log.sb_lines[slot]);
        c->log.sb_head = (c->log.sb_head + 1) % ZT_SCROLLBACK_CAP;
    }
    c->log.sb_lines[slot] = s;
}

void scrollback_free(zt_ctx *c) {
    if (!c->log.sb_lines) return;
    for (int i = 0; i < ZT_SCROLLBACK_CAP; i++)
        free(c->log.sb_lines[i]);
    free(c->log.sb_lines);
    c->log.sb_lines = NULL;
}
