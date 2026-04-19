/**
 * @file pager.c
 * @brief less-style pager keys while scrolled back.
 *
 * When the scrollback view is active (c->sb_view_offset > 0), route
 * keys through this handler first:
 *
 *   g       → top of scrollback
 *   G       → bottom (leave)
 *   /       → start search
 *   n / N   → next / prev match
 *   q       → quit
 *   space   → page down
 *   b       → page up
 *   j / k   → line down / line up
 *
 * Returns true when it consumed the key.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

bool pager_handle(zt_ctx *c, unsigned char k) {
    if (!c || !c->tui.pager_mode) return false;
    if (c->tui.sb_offset <= 0 && k != 'q') return false;
    switch (k) {
    case 'q':
        c->tui.pager_mode = false;
        leave_scroll(c);
        return true;
    case ' ': scroll_up(c, c->tui.rows - 3); return true;
    case 'b': scroll_down(c, c->tui.rows - 3); return true;
    case 'j': scroll_up(c, 1); return true;
    case 'k': scroll_down(c, 1); return true;
    case 'g':
        c->tui.sb_offset = c->log.sb_count;
        redraw_scrollback(c);
        return true;
    case 'G': leave_scroll(c); return true;
    case '/':
        c->tui.search_mode = true;
        c->tui.search_len  = 0;
        draw_search_bar(c);
        return true;
    case 'n': search_scrollback(c, +1); return true;
    case 'N': search_scrollback(c, -1); return true;
    default: return false;
    }
}
