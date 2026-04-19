/**
 * @file fuzzy.c
 * @brief Incremental fuzzy finder over history + scrollback.
 *
 * Ctrl+A / enters fuzzy mode; typing filters matches in real time using
 * a cheap subsequence scorer (fzf-compatible ordering: earlier matches
 * and consecutive runs score higher). Enter selects, Esc cancels.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <ctype.h>
#include <string.h>

void fuzzy_enter(zt_ctx *c) {
    if (!c) return;
    c->tui.fuzzy_mode     = true;
    c->tui.fuzzy_len      = 0;
    c->tui.fuzzy_buf[0]   = 0;
    c->tui.fuzzy_selected = 0;
    (void)c;
}
void fuzzy_exit(zt_ctx *c) {
    if (!c) return;
    c->tui.fuzzy_mode = false;
}

static int score(const char *needle, const char *hay) {
    int         s = 0, streak = 0;
    const char *h = hay;
    for (const char *n = needle; *n; n++) {
        unsigned char tn = (unsigned char)tolower((unsigned char)*n);
        while (*h && (unsigned char)tolower((unsigned char)*h) != tn) {
            h++;
            streak = 0;
        }
        if (!*h) return -1;
        streak++;
        s += 1 + streak;
        h++;
    }
    return s;
}

bool fuzzy_handle(zt_ctx *c, unsigned char k) {
    if (!c || !c->tui.fuzzy_mode) return false;
    if (k == 27) {
        fuzzy_exit(c);
        return true;
    }
    if (k == '\r' || k == '\n') {
        /* inject the currently selected string */
        const char *sel = history_at(c, c->tui.fuzzy_selected);
        if (sel && *sel) {
            /* copy into input buffer */
            size_t n = strlen(sel);
            if (n > sizeof c->tui.input_buf) n = sizeof c->tui.input_buf - 1;
            memcpy(c->tui.input_buf, sel, n);
            c->tui.input_len = n;
            c->tui.cursor    = n;
        }
        fuzzy_exit(c);
        return true;
    }
    if (k == 0x7F || k == 0x08) {
        if (c->tui.fuzzy_len > 0) c->tui.fuzzy_buf[--c->tui.fuzzy_len] = 0;
        return true;
    }
    if (k >= 32 && k < 127 && c->tui.fuzzy_len < sizeof c->tui.fuzzy_buf - 1) {
        c->tui.fuzzy_buf[c->tui.fuzzy_len++] = (char)k;
        c->tui.fuzzy_buf[c->tui.fuzzy_len]   = 0;
    }
    /* pick best history match */
    int best = 0, best_score = -1;
    for (int i = 0; i < 64; i++) {
        const char *h = history_at(c, i);
        if (!h || !*h) break;
        int s = score(c->tui.fuzzy_buf, h);
        if (s > best_score) {
            best_score = s;
            best       = i;
        }
    }
    c->tui.fuzzy_selected = best;
    return true;
}

void fuzzy_draw(zt_ctx *c) {
    if (!c || !c->tui.fuzzy_mode) return;
    const char *hit = history_at(c, c->tui.fuzzy_selected);
    char        line1[256], line2[256];
    snprintf(line1, sizeof line1, "  query: %s", c->tui.fuzzy_buf);
    snprintf(line2, sizeof line2, "  match: %s", hit ? hit : "(no match)");
    const char *body[] = {line1, line2};
    draw_dialog(c, "\xef\x80\x82", " FUZZY ", "\x1b[38;5;141m", body, 2,
                "[Enter] select  [Esc] cancel");
}
