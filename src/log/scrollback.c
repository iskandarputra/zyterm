/**
 * @file    scrollback.c
 * @brief   Scrollback ring buffer and viewport management.
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

/* ------------------------------ scrollback ------------------------------- */

/* forward decl — defined in RX rendering section */
const char *zephyr_color(const unsigned char *line, size_t len);

/* ---------------------- selection geometry helpers ----------------------- */

/* Skip a single ANSI/OSC escape sequence starting at `s+i` (where
 * `s[i]` is the leading 0x1b). Returns the new byte offset after the
 * full sequence so column counters can step over invisible bytes
 * without counting them as columns. We recognise:
 *   ESC [ … final         (CSI: final byte in 0x40-0x7E, e.g. SGR `m`)
 *   ESC ] … BEL | ESC \\  (OSC: BEL- or ST-terminated)
 *   ESC X (single byte)   (any other 2-byte escape, conservative skip)
 * `len` is the upper bound; we never read past it. */
static size_t skip_esc(const char *s, size_t len, size_t i) {
    if (i >= len || (unsigned char)s[i] != 0x1b) return i + 1;
    size_t j = i + 1;
    if (j >= len) return len;
    char kind = s[j++];
    if (kind == '[') {
        while (j < len) {
            unsigned char b = (unsigned char)s[j++];
            if (b >= 0x40 && b <= 0x7E) break;
        }
    } else if (kind == ']') {
        while (j < len) {
            unsigned char b = (unsigned char)s[j];
            if (b == 0x07) {
                j++;
                break;
            }
            if (b == 0x1b && j + 1 < len && s[j + 1] == '\\') {
                j += 2;
                break;
            }
            j++;
        }
    }
    return j;
}

/* Map a 1-based column to a byte offset in a UTF-8 string.
 * One column per codepoint (no East-Asian-wide handling). Skips ANSI
 * SGR/OSC escape sequences so embedded colours don't shift the
 * mapping. On overshoot returns `len`. */
static size_t col_to_byte(const char *s, size_t len, int col_target) {
    if (col_target <= 0 || !s) return 0;
    size_t i   = 0;
    int    col = 0;
    while (i < len && col < col_target) {
        unsigned char b = (unsigned char)s[i];
        if (b == 0x1b) {
            i = skip_esc(s, len, i);
            continue;
        }
        size_t step = 1;
        if ((b & 0x80) == 0)
            step = 1;
        else if ((b & 0xE0) == 0xC0)
            step = 2;
        else if ((b & 0xF0) == 0xE0)
            step = 3;
        else if ((b & 0xF8) == 0xF0)
            step = 4;
        if (i + step > len) step = len - i;
        i += step;
        col++;
    }
    return i;
}

/* Display-column count of a UTF-8 string (one column per codepoint).
 * Skips ANSI SGR/OSC escape sequences. */
static int utf8_cols(const char *s, size_t len) {
    int    col = 0;
    size_t i   = 0;
    while (i < len) {
        unsigned char b = (unsigned char)s[i];
        if (b == 0x1b) {
            i = skip_esc(s, len, i);
            continue;
        }
        size_t step = 1;
        if ((b & 0x80) == 0)
            step = 1;
        else if ((b & 0xE0) == 0xC0)
            step = 2;
        else if ((b & 0xF0) == 0xE0)
            step = 3;
        else if ((b & 0xF8) == 0xF0)
            step = 4;
        if (i + step > len) break;
        i += step;
        col++;
    }
    return col;
}

/* Order the selection endpoints into (first_line, first_col, last_line,
 * last_col) where "first" is the older / higher-line-from-bottom end.
 * This is the visual top of the highlight: scrollback grows downward in
 * time so the line with the LARGER line_from_bottom is older = on top. */
static void selection_ordered(const zt_ctx *c, int *fl, int *fc, int *ll, int *lc) {
    int al = c->tui.sel_anchor_line, ac = c->tui.sel_anchor_col;
    int el = c->tui.sel_end_line, ec = c->tui.sel_end_col;
    /* "first" = visually-higher row = larger line_from_bottom. */
    if (al > el || (al == el && ac < ec)) {
        *fl = al;
        *fc = ac;
        *ll = el;
        *lc = ec;
    } else {
        *fl = el;
        *fc = ec;
        *ll = al;
        *lc = ac;
    }
}

/* Translate body-region screen row → line_from_bottom (0 = newest).
 * Returns -1 if the row is outside the body region. The mapped line may
 * still be off the end of the available history; callers should check. */
static int row_to_line_from_bottom(const zt_ctx *c, int screen_row) {
    int vis = c->tui.rows - 2;
    if (vis < 1) return -1;
    int r_in_vis = screen_row - 2;
    if (r_in_vis < 0 || r_in_vis >= vis) return -1;
    return c->tui.sb_offset + (vis - 1 - r_in_vis);
}

/* Translate line_from_bottom → sb_lines[] index. Returns -1 if past EOF. */
static int line_from_bottom_to_sb_index(const zt_ctx *c, int line_from_bottom) {
    if (line_from_bottom < 0 || line_from_bottom >= c->log.sb_count) return -1;
    int idx = (c->log.sb_head + c->log.sb_count - 1 - line_from_bottom) % ZT_SCROLLBACK_CAP;
    if (idx < 0) idx += ZT_SCROLLBACK_CAP;
    return idx;
}

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

/* Draw the scroll region from the scrollback buffer at the current offset.
 * When sb_offset == 0 this shows the most recent lines (live tail). */
void redraw_scrollback(zt_ctx *c) {
    int vis = c->tui.rows - 2;
    if (vis <= 0) return;

    /* Pre-compute selection bounds once (in ordered form) so each row can
     * cheaply test whether it falls inside without doing an anchor swap
     * on every iteration. Bounds are stored as line_from_bottom values
     * which are stable across scrolls. */
    int  sel_fl = 0, sel_fc = 0, sel_ll = 0, sel_lc = 0;
    bool have_sel = c->tui.sel_active;
    if (have_sel) selection_ordered(c, &sel_fl, &sel_fc, &sel_ll, &sel_lc);

    for (int r = 0; r < vis; r++) {
        int  row_num          = r + 2;
        int  line_from_bottom = c->tui.sb_offset + (vis - 1 - r);

        char mv[32];
        snprintf(mv, sizeof mv, "\033[%d;1H\033[2K", row_num);
        ob_cstr(mv);

        const char *line     = NULL;
        size_t      line_len = 0;
        if (line_from_bottom < c->log.sb_count) {
            int idx =
                (c->log.sb_head + c->log.sb_count - 1 - line_from_bottom) % ZT_SCROLLBACK_CAP;
            if (idx < 0) idx += ZT_SCROLLBACK_CAP;
            line = c->log.sb_lines[idx];
            if (line) {
                line_len = strlen(line);
                emit_colored_line(c, (const unsigned char *)line, line_len);
            }
        }

        /* Selection overlay: re-paint the selected substring of this row
         * in reverse video on top of the line we just drew.
         * Selection is keyed off line_from_bottom (sel_fl/sel_ll), so
         * as the user scrolls the highlight stays glued to the actual
         * scrollback lines instead of to fixed screen rows.
         *
         * The slice may contain embedded SGR escapes from the source
         * line (e.g. `\033[33m<inf>\033[0m …`). Their `\033[0m` would
         * cancel our `\033[7m` mid-row and leave a visual gap after each
         * coloured token. We strip ALL escapes from the highlighted
         * slice so the whole selection renders as a single reverse-video
         * span — same as gnome-terminal/iTerm/etc. */
        if (have_sel && line_from_bottom <= sel_fl && line_from_bottom >= sel_ll && line &&
            line_len > 0) {
            /* When timestamps are hidden, the on-screen layout skips the
             * embedded [HH:MM:SS.mmm] prefix (15 chars). Map selection
             * columns against the same visible portion so the highlight
             * overlay aligns with what emit_colored_line() actually drew
             * and hidden timestamps don't leak into the reverse-video
             * span. */
            size_t      ts_off   = 0;
            if (!c->proto.show_ts && line_len >= 15 &&
                line[0] == '[' && line[3] == ':' && line[6] == ':' &&
                line[9] == '.' && line[13] == ']' && line[14] == ' ')
                ts_off = 15;
            const char *vis      = line + ts_off;
            size_t      vis_len  = line_len - ts_off;
            int total_cols = utf8_cols(vis, vis_len);
            int sc         = (line_from_bottom == sel_fl) ? sel_fc : 1;
            int ec         = (line_from_bottom == sel_ll) ? sel_lc : total_cols;
            if (sc < 1) sc = 1;
            if (ec > total_cols) ec = total_cols;
            if (ec >= sc && total_cols > 0) {
                size_t bs = col_to_byte(vis, vis_len, sc - 1);
                size_t be = col_to_byte(vis, vis_len, ec);
                if (be > bs) {
                    char pos[32];
                    snprintf(pos, sizeof pos, "\033[%d;%dH\033[7m", row_num, sc);
                    ob_cstr(pos);
                    /* Stream the slice byte-by-byte, skipping any
                     * embedded ESC sequence. Cheaper than allocating
                     * a stripped copy per row. */
                    size_t i = bs;
                    while (i < be) {
                        if ((unsigned char)vis[i] == 0x1b) {
                            i = skip_esc(vis, vis_len, i);
                            continue;
                        }
                        ob_write(vis + i, 1);
                        i++;
                    }
                    ob_cstr("\033[27m");
                }
            }
        }

        /* Scrollbar on right edge.
         *
         * Geometry:
         *   travel = vis - bar_h        (room the thumb can move in)
         *   maxoff = sb_count - vis     (maximum scroll-back distance)
         *   sb_offset == 0       → live tail   → thumb at bottom
         *   sb_offset == maxoff  → oldest line → thumb at top
         *
         * `sb_offset` is mapped directly onto a fixed travel band so
         * both endpoints align exactly with the live tail and the top
         * of history regardless of how `sb_count` and `vis` relate. */
        if (c->log.sb_count > vis) {
            int bar_h = vis * vis / c->log.sb_count;
            if (bar_h < 1) bar_h = 1;
            if (bar_h > vis) bar_h = vis;
            int travel = vis - bar_h;
            int maxoff = c->log.sb_count - vis;
            if (maxoff < 1) maxoff = 1;
            int bar_top = travel - (c->tui.sb_offset * travel / maxoff);
            if (bar_top < 0) bar_top = 0;
            if (bar_top > travel) bar_top = travel;
            char sc[32];
            snprintf(sc, sizeof sc, "\033[%d;%dH", row_num, c->tui.cols);
            ob_cstr(sc);
            if (r >= bar_top && r < bar_top + bar_h)
                ob_cstr("\033[38;5;111m\xe2\x96\x90\033[0m"); /* ▐ thumb */
            else
                ob_cstr("\033[38;5;237m\xe2\x94\x82\033[0m"); /* │ track */
        }
    }
}

void scroll_up(zt_ctx *c, int lines) {
    int vis    = c->tui.rows - 2;
    int maxoff = c->log.sb_count - vis;
    if (maxoff < 0) maxoff = 0;
    c->tui.sb_offset += lines;
    if (c->tui.sb_offset > maxoff) c->tui.sb_offset = maxoff;
    /* Selection is anchored to scrollback line indices, not screen rows,
     * so it stays attached to the right text across scrolls — no need
     * to clear it here. (Matches minicom / tmux copy-mode behaviour.) */
    c->tui.sb_redraw = true;
    c->tui.ui_dirty  = true;
}

void scroll_down(zt_ctx *c, int lines) {
    c->tui.sb_offset -= lines;
    if (c->tui.sb_offset < 0) c->tui.sb_offset = 0;
    c->tui.sb_redraw = true;
    c->tui.ui_dirty  = true;
}

void leave_scroll(zt_ctx *c) {
    if (c->tui.sb_offset == 0) return;
    c->tui.sb_offset = 0;
    c->tui.sb_redraw = true;
    c->tui.ui_dirty  = true;
}

/* ------------------------------- selection ------------------------------- */

void selection_clear(zt_ctx *c) {
    if (!c->tui.sel_active && !c->tui.sel_dragging) return;
    c->tui.sel_active   = false;
    c->tui.sel_dragging = false;
    c->tui.sb_redraw    = true;
    c->tui.ui_dirty     = true;
}

void selection_begin(zt_ctx *c, int row, int col) {
    int line = row_to_line_from_bottom(c, row);
    if (line < 0) line = 0;
    c->tui.sel_anchor_line = line;
    c->tui.sel_anchor_col  = col;
    c->tui.sel_end_line    = line;
    c->tui.sel_end_col     = col;
    c->tui.sel_dragging    = true;
    /* sel_active stays false until the first motion event. A pure click
     * (press + release with no drag) just clears any prior selection. */
    bool had          = c->tui.sel_active;
    c->tui.sel_active = false;
    if (had) {
        c->tui.sb_redraw = true;
        c->tui.ui_dirty  = true;
    }
}

void selection_extend(zt_ctx *c, int row, int col) {
    if (!c->tui.sel_dragging) return;
    int line = row_to_line_from_bottom(c, row);
    if (line < 0) line = 0;
    if (line == c->tui.sel_end_line && col == c->tui.sel_end_col && c->tui.sel_active) return;
    c->tui.sel_end_line = line;
    c->tui.sel_end_col  = col;
    c->tui.sel_active   = true;
    c->tui.sb_redraw    = true;
    c->tui.ui_dirty     = true;
}

/* Strip CSI / OSC escape sequences in-place so the clipboard receives
 * pure visible text. Conservative: only drops well-formed ESC[..final
 * and ESC]..(BEL|ESC\\) runs; leaves any other byte untouched. */
static size_t strip_escapes(char *s, size_t n) {
    size_t r = 0, w = 0;
    while (r < n) {
        unsigned char b = (unsigned char)s[r];
        if (b == 0x1B && r + 1 < n) {
            unsigned char nx = (unsigned char)s[r + 1];
            if (nx == '[') { /* CSI: ESC [ ... <0x40-0x7E> */
                r += 2;
                while (r < n) {
                    unsigned char ch = (unsigned char)s[r++];
                    if (ch >= 0x40 && ch <= 0x7E) break;
                }
                continue;
            }
            if (nx == ']') { /* OSC: ESC ] ... (BEL | ESC \) */
                r += 2;
                while (r < n) {
                    unsigned char ch = (unsigned char)s[r++];
                    if (ch == 0x07) break;
                    if (ch == 0x1B && r < n && (unsigned char)s[r] == '\\') {
                        r++;
                        break;
                    }
                }
                continue;
            }
            /* Unknown two-byte ESC sequence \u2014 drop both bytes. */
            r += 2;
            continue;
        }
        s[w++] = (char)b;
        r++;
    }
    return w;
}

void selection_copy(zt_ctx *c) {
    if (!c->tui.sel_active) return;
    int sel_fl, sel_fc, sel_ll, sel_lc;
    selection_ordered(c, &sel_fl, &sel_fc, &sel_ll, &sel_lc);

    /* Build the selected text by walking each covered scrollback line
     * (sel_fl is the older / visually-higher end \u2014 larger
     * line_from_bottom \u2014 and sel_ll is the newer / lower end). Cap at
     * 256 KiB so a wild drag across a giant scrollback can\u2019t blow
     * the heap. */
    size_t cap = 65536, len = 0;
    char  *out = malloc(cap);
    if (!out) return;

    for (int line = sel_fl; line >= sel_ll; line--) {
        int idx = line_from_bottom_to_sb_index(c, line);
        if (idx < 0) {
            if (line != sel_ll) {
                if (len + 1 >= cap) {
                    cap *= 2;
                    char *t = realloc(out, cap);
                    if (!t) {
                        free(out);
                        return;
                    }
                    out = t;
                }
                out[len++] = '\n';
            }
            continue;
        }
        const char *s = c->log.sb_lines[idx];
        if (!s) s = "";
        size_t s_len = strlen(s);
        /* Skip hidden timestamp prefix so column mapping matches the
         * visible layout (same logic as emit_colored_line / overlay). */
        size_t ts_off = 0;
        if (!c->proto.show_ts && s_len >= 15 &&
            s[0] == '[' && s[3] == ':' && s[6] == ':' &&
            s[9] == '.' && s[13] == ']' && s[14] == ' ')
            ts_off = 15;
        const char *vis     = s + ts_off;
        size_t      vis_len = s_len - ts_off;
        int    total = utf8_cols(vis, vis_len);
        int    sc    = (line == sel_fl) ? sel_fc : 1;
        int    ec    = (line == sel_ll) ? sel_lc : total;
        if (sc < 1) sc = 1;
        if (ec > total) ec = total;

        size_t bs = (ec < sc || total == 0) ? 0 : col_to_byte(vis, vis_len, sc - 1);
        size_t be = (ec < sc || total == 0) ? 0 : col_to_byte(vis, vis_len, ec);
        if (be > bs) {
            size_t need = (be - bs) + 1; /* +1 for possible trailing \n */
            while (len + need >= cap) {
                if (cap >= 256 * 1024) {
                    be   = bs + (256 * 1024 - len - 1);
                    need = (be - bs) + 1;
                    break;
                }
                cap *= 2;
                char *t = realloc(out, cap);
                if (!t) {
                    free(out);
                    return;
                }
                out = t;
            }
            memcpy(out + len, vis + bs, be - bs);
            len += (be - bs);
        }
        if (line != sel_ll) {
            if (len + 1 >= cap) {
                cap += 64;
                char *t = realloc(out, cap);
                if (!t) {
                    free(out);
                    return;
                }
                out = t;
            }
            out[len++] = '\n';
        }
        if (len >= 256 * 1024) break;
    }

    len = strip_escapes(out, len);
    /* osc52_copy emits its own "copied N bytes" flash. If OSC 52 is
     * disabled (e.g. the user passed --no-osc52) the helper is a no-op
     * \u2014 still surface a confirmation so the user knows the selection
     * was registered. */
    if (c->proto.osc52_enabled) {
        osc52_copy(c, out, len);
    } else {
        set_flash(c, "selection: %zu bytes (OSC 52 disabled \u2014 enable to copy)", len);
    }
    free(out);
}

void selection_finish(zt_ctx *c) {
    if (!c->tui.sel_dragging) return;
    c->tui.sel_dragging = false;
    if (c->tui.sel_active) selection_copy(c);
}
