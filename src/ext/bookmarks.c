/**
 * @file bookmarks.c
 * @brief Scrollback bookmarks — named anchors in RX history.
 *
 * Press Ctrl+A B to add a bookmark at the current scrollback position;
 * the HUD shows a small pin icon and you can cycle through bookmarks
 * with Alt-[ / Alt-] (or a dedicated dialog via Ctrl+A L).
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"
#include <stdlib.h>
#include <string.h>

int bookmark_add(zt_ctx *c, int sb_offset, const char *note) {
    if (!c) return -1;
    if (c->log.bookmark_count >= ZT_BOOKMARK_MAX) {
        /* rotate out the oldest */
        free(c->log.bookmark_notes[0]);
        memmove(c->log.bookmarks, c->log.bookmarks + 1,
                (ZT_BOOKMARK_MAX - 1) * sizeof c->log.bookmarks[0]);
        memmove(c->log.bookmark_notes, c->log.bookmark_notes + 1,
                (ZT_BOOKMARK_MAX - 1) * sizeof c->log.bookmark_notes[0]);
        c->log.bookmark_count = ZT_BOOKMARK_MAX - 1;
    }
    c->log.bookmarks[c->log.bookmark_count]      = sb_offset;
    c->log.bookmark_notes[c->log.bookmark_count] = note ? strdup(note) : NULL;
    c->log.bookmark_count++;
    set_flash(c, "bookmark #%d added", c->log.bookmark_count);
    return c->log.bookmark_count - 1;
}

void bookmark_remove(zt_ctx *c, int idx) {
    if (!c || idx < 0 || idx >= c->log.bookmark_count) return;
    free(c->log.bookmark_notes[idx]);
    memmove(c->log.bookmarks + idx, c->log.bookmarks + idx + 1,
            (c->log.bookmark_count - idx - 1) * sizeof c->log.bookmarks[0]);
    memmove(c->log.bookmark_notes + idx, c->log.bookmark_notes + idx + 1,
            (c->log.bookmark_count - idx - 1) * sizeof c->log.bookmark_notes[0]);
    c->log.bookmark_count--;
}

int bookmark_goto(zt_ctx *c, int idx) {
    if (!c || idx < 0 || idx >= c->log.bookmark_count) return -1;
    c->tui.sb_offset = c->log.bookmarks[idx];
    redraw_scrollback(c);
    return 0;
}

void bookmark_list_draw(zt_ctx *c) {
    if (!c || c->log.bookmark_count == 0) {
        set_flash(c, "no bookmarks");
        return;
    }
    char   buf[1024];
    size_t o = 0;
    for (int i = 0; i < c->log.bookmark_count && o + 80 < sizeof buf; i++) {
        o += (size_t)snprintf(
            buf + o, sizeof buf - o, "%d: @%d %s\n", i + 1, c->log.bookmarks[i],
            c->log.bookmark_notes[i] ? c->log.bookmark_notes[i] : "(no note)");
    }
    const char *lines[ZT_BOOKMARK_MAX + 1] = {0};
    char       *p                          = buf;
    int         n                          = 0;
    while (*p && n < ZT_BOOKMARK_MAX) {
        lines[n++] = p;
        char *nl   = strchr(p, '\n');
        if (!nl) break;
        *nl = 0;
        p   = nl + 1;
    }
    draw_dialog(c, "\xef\x80\x97", " BOOKMARKS ", "\x1b[38;5;214m", lines, n,
                "[#] goto  [Ctrl+D] delete  [Esc] close");
}
