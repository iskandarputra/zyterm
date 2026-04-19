/**
 * @file search.c
 * @brief Scrollback search + rename-log mini prompts
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

/* ------------------------------ search ----------------------------------- */

/* Scan scrollback for search_buf; dir = +1 older, -1 newer.
 * Returns 1 on hit (sb_offset updated), 0 on miss. */
int search_scrollback(zt_ctx *c, int dir) {
    if (c->tui.search_len == 0 || c->log.sb_count == 0) return 0;
    int vis = c->tui.rows - 2;
    if (vis < 1) vis = 1;
    int maxoff = c->log.sb_count - 1;
    int start  = c->tui.sb_offset + (dir > 0 ? 1 : -1);
    for (int off = start; off >= 0 && off <= maxoff; off += dir) {
        int idx = (c->log.sb_head + c->log.sb_count - 1 - off) % ZT_SCROLLBACK_CAP;
        if (idx < 0) idx += ZT_SCROLLBACK_CAP;
        const char *line = c->log.sb_lines[idx];
        if (!line) continue;
        if (strstr(line, c->tui.search_buf)) {
            /* centre the match in the scroll region */
            c->tui.sb_offset = off - vis / 2;
            if (c->tui.sb_offset < 0) c->tui.sb_offset = 0;
            int mx = c->log.sb_count - vis;
            if (mx < 0) mx = 0;
            if (c->tui.sb_offset > mx) c->tui.sb_offset = mx;
            return 1;
        }
    }
    return 0;
}

void draw_search_bar(zt_ctx *c) {
    if (c->tui.rows < 2) return;
    char head[32];
    int  hn = snprintf(head, sizeof head, "\033[%d;1H\033[2K", c->tui.rows);
    if (hn > 0) ob_write(head, (size_t)hn);
    ob_cstr("\033[38;5;178m\xe2\x96\x8e\033[1;38;5;214m SEARCH \033[0m ");
    ob_cstr("\033[1;38;5;252m");
    ob_write(c->tui.search_buf, c->tui.search_len);
    ob_cstr("\033[0m\033[?25h");
    ob_flush();
}

/* Bottom-line prompt for renaming the active log. Shows the current filename
 * as a dim placeholder until the user begins typing. Esc cancels, Enter
 * applies the rename via rename(2). */
void draw_rename_bar(zt_ctx *c) {
    char head[32];
    int  hn = snprintf(head, sizeof head, "\033[%d;1H\033[2K", c->tui.rows);
    if (hn > 0) ob_write(head, (size_t)hn);
    ob_cstr("\033[38;5;172m\xe2\x96\x8e\033[1;38;5;178m RENAME \033[0m ");
    if (c->tui.rename_len == 0 && c->log.path) {
        ob_cstr("\033[2;38;5;243m");
        ob_cstr(c->log.path);
        ob_cstr("\033[0m \033[38;5;239m\xe2\x80\xa2\033[38;5;243m type new name, Esc to "
                "cancel\033[0m");
    } else {
        ob_cstr("\033[1;38;5;252m");
        ob_write(c->tui.rename_buf, c->tui.rename_len);
        ob_cstr("\033[0m");
    }
    ob_cstr("\033[?25h");
    ob_flush();
}
