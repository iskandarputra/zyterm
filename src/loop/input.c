/**
 * @file input.c
 * @brief Keyboard + escape + mouse + command-mode dispatch
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

/* ------------------------------ stdin handling --------------------------- */

void load_history_into_buf(zt_ctx *c) {
    const char *s = history_at(c, c->tui.hist_view);
    if (!s) return;
    size_t n = strlen(s);
    if (n >= ZT_INPUT_CAP) n = ZT_INPUT_CAP - 1;
    memcpy(c->tui.input_buf, s, n);
    c->tui.input_len = n;
    c->tui.sent_len  = 0;
    c->tui.cursor    = n;
}

void handle_cmd_key(zt_ctx *c, unsigned char k) {
    c->tui.command_mode = false;
    c->tui.popup_active = false;
    switch (k) {
    case 'x':
    case 'X': zt_g_quit = 1; break;
    case 'p':
    case 'P':
        c->core.paused = !c->core.paused;
        set_flash(c, c->core.paused ? "stdout PAUSED (log still writing)" : "resumed");
        break;
    case 'e':
    case 'E':
        c->proto.local_echo = !c->proto.local_echo;
        set_flash(c, "local echo %s", c->proto.local_echo ? "on" : "off");
        break;
    case 'h':
    case 'H':
        c->log.hex_mode = !c->log.hex_mode;
        if (c->log.hex_mode) {
            /* Entering hex mode: flush any pending text line, then
             * reset the hex accumulator so we start a fresh dump. */
            if (c->log.line_len) flush_line(c);
            c->log.hex_col     = 0;
            c->log.hex_row_len = 0;
            c->log.hex_offset  = 0;
        } else {
            /* Leaving hex mode: flush any partial hex row so it
             * appears in scrollback before we switch back to text. */
            hex_flush_row(c);
        }
        set_flash(c, "hex %s", c->log.hex_mode ? "on" : "off");
        break;
    case 't':
    case 'T':
        c->proto.show_ts = !c->proto.show_ts;
        c->tui.sb_redraw = true; /* repaint scrollback: show/hide timestamps */
        set_flash(c, "timestamps %s", c->proto.show_ts ? "on" : "off");
        break;
    case 'c':
    case 'C':
        c->log.line_len = 0;
        c->log.hex_col  = 0;
        apply_layout(c);
        return;
    case 'b':
    case 'B':
        if (tcsendbreak(c->serial.fd, 0) == 0)
            set_flash(c, "break sent");
        else
            set_flash(c, "break failed: %s", strerror(errno));
        break;
    case 'a':
    case 'A': {
        unsigned char x = 0x01;
        direct_send(c, &x, 1);
        break;
    }
    case 's':
    case 'S': {
        struct timespec t;
        now(&t);
        double el = ts_diff_sec(&t, &c->core.t_start);
        log_notice(c, "stats: RX=%llu TX=%llu lines=%llu uptime=%.1fs avg=%.0f B/s",
                   (unsigned long long)c->core.rx_bytes, (unsigned long long)c->core.tx_bytes,
                   (unsigned long long)c->core.rx_lines, el,
                   el > 0 ? (double)c->core.rx_bytes / el : 0.0);
        break;
    }
    case 'l':
    case 'L': {
        /* Toggle logging. When enabling, auto-name:
         *   zyterm-YYYYMMDD-NNN.txt   (NNN = next free index for today). */
        if (c->log.fd >= 0) {
            close(c->log.fd);
            c->log.fd = -1;
            set_flash(c, "logging stopped");
        } else {
            static char log_name_buf[PATH_MAX_LEN];
            time_t      nw = time(NULL);
            struct tm   tm;
            localtime_r(&nw, &tm);
            int found = 0;
            for (int i = 1; i < 1000; i++) {
                snprintf(log_name_buf, sizeof log_name_buf, "zyterm-%04d%02d%02d-%03d.txt",
                         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, i);
                if (access(log_name_buf, F_OK) != 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                set_flash(c, "log: ran out of names for today");
                break;
            }
            int fd = open(log_name_buf, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
            if (fd < 0) {
                set_flash(c, "log open failed: %s", strerror(errno));
            } else {
                c->log.fd         = fd;
                c->log.bytes      = 0;
                c->log.line_start = true;
                c->log.path       = log_name_buf;
                set_flash(c, "logging \xe2\x86\x92 %s", log_name_buf);
            }
        }
        break;
    }
    case 'f': {
        c->serial.flow = (c->serial.flow + 1) % 3;
        const char *name =
            c->serial.flow == 0 ? "none" : (c->serial.flow == 1 ? "RTS/CTS" : "XON/XOFF");
        if (c->serial.fd >= 0 && apply_flow(c->serial.fd, c->serial.flow) == 0)
            set_flash(c, "flow control: %s", name);
        else
            set_flash(c, "flow control: %s (apply failed)", name);
        break;
    }
    case 'r':
    case 'R': {
        /* Manual reconnect: close then enter the full wait-and-retry loop
         * so the user sees the reconnecting spinner until the device is
         * back (or they quit). Single-shot would leave serial_fd == -1. */
        if (c->serial.fd >= 0) {
            close(c->serial.fd);
            c->serial.fd = -1;
        }
        if (reconnect_attempt(c) == 0) {
            set_flash(c, "reconnected to %s", c->serial.device);
        } else if (c->core.reconnect) {
            run_reconnect_loop(c);
        } else {
            set_flash(c, "reconnect failed: %s", strerror(errno));
        }
        break;
    }
    case 'm':
    case 'M': {
        /* Toggle DEC mouse tracking modes ?1000 (button events),
         * ?1002 (drag motion) and ?1006 (SGR extended coordinates).
         *
         *   ON  (default) \u2014 zyterm consumes click/drag/wheel events to
         *                   drive in-app selection (drag \u2192 highlight \u2192
         *                   clipboard via OSC 52 + native X11 owner),
         *                   wheel scroll, and scrollbar drag. Native
         *                   host-terminal text selection requires
         *                   holding Shift to bypass tracking.
         *   OFF           — host terminal owns the mouse: native shift-
         *                   free click+drag selection, but in-app
         *                   scrollbar and wheel scrolling are inert. */
        c->tui.mouse_tracking = !c->tui.mouse_tracking;
        (void)zt_write_cstr(STDOUT_FILENO, c->tui.mouse_tracking
                                               ? "\033[?1000h\033[?1002h\033[?1006h"
                                               : "\033[?1000l\033[?1002l\033[?1006l");
        set_flash(c, c->tui.mouse_tracking
                         ? "mouse capture ON · in-app scroll/wheel · Shift+drag to select"
                         : "mouse capture OFF · native select + scroll-while-dragging");
        break;
    }
    case 'n':
    case 'N': {
        /* Enter rename-log mode. Only meaningful when a log is active. */
        if (c->log.fd < 0 || !c->log.path) {
            set_flash(c, "no active log to rename (Ctrl+A l to start one)");
            break;
        }
        c->tui.rename_mode   = true;
        c->tui.rename_len    = 0;
        c->tui.rename_buf[0] = 0;
        draw_rename_bar(c);
        return;
    }
    case '/':
        c->tui.search_mode    = true;
        c->tui.search_len     = 0;
        c->tui.search_buf[0]  = 0;
        c->tui.search_hits    = 0;
        c->tui.search_current = 0;
        draw_search_bar(c);
        return;
    case 'k': draw_keybind_popup(c); return; /* stay in popup mode — don't apply_layout */
    case 'j':
    case 'J':
        c->log.format = (c->log.format + 1) % ZT_LOG__COUNT;
        set_flash(c, "log format: %s",
                  c->log.format == ZT_LOG_JSON  ? "json"
                  : c->log.format == ZT_LOG_RAW ? "raw"
                                                : "text");
        break;
    case 'F': /* NOTE: lowercase 'f' is already flow-control above.
               * Uppercase F cycles framing mode. */
        c->proto.mode = (c->proto.mode + 1) % ZT_FRAME__COUNT;
        framing_reset(c);
        set_flash(c, "frame mode: %s", framing_name(c->proto.mode));
        break;
    case 'K': /* (K)heck-sum — 'C' was clear, 'K' for CRC cycle */
        c->proto.crc_mode = (c->proto.crc_mode + 1) % ZT_CRC__COUNT;
        set_flash(c, "crc mode: %s", crc_name(c->proto.crc_mode));
        break;
    case 'G':
        if (c->proto.passthrough)
            passthrough_exit(c);
        else
            passthrough_enter(c);
        break;
    case '+': /* '+' adds a bookmark (lowercase 'b' is break) */
        bookmark_add(c, c->tui.sb_offset, NULL);
        break;
    case '[': bookmark_list_draw(c); return;
    case 'Y':
        /* Yank to clipboard. Prefer the active mouse selection (so the
         * keyboard works as a backup when the user’s host terminal
         * silently swallowed our OSC 52 push at drag-release), then fall
         * back to the current line still being assembled in log_line.
         * Right-click on a selection does the same thing — think of
         * Ctrl+A Y as the keyboard equivalent. */
        if (c->tui.sel_active) {
            selection_copy(c);
        } else if (c->log.line_len > 0) {
            osc52_copy(c, (const char *)c->log.line, c->log.line_len);
        } else {
            set_flash(c, "nothing to copy (drag to select, or wait for a line)");
        }
        break;
    case '.':
        fuzzy_enter(c);
        fuzzy_draw(c);
        return;
    case 'D':
        c->log.mute_dbg = !c->log.mute_dbg;
        set_flash(c, "<dbg> %s", c->log.mute_dbg ? "muted" : "shown");
        break;
    case 'I':
        c->log.mute_inf = !c->log.mute_inf;
        set_flash(c, "<inf> %s", c->log.mute_inf ? "muted" : "shown");
        break;
    case 'Q':
        if (c->serial.fd >= 0) {
            close(c->serial.fd);
            c->serial.fd = -1;
        }
        autobaud_probe(c);
        break;
    case 'o':
    case 'O':
        c->tui.settings_mode = true;
        c->tui.settings_page = 0;
        draw_settings_page(c);
        return; /* skip apply_layout — popup is on-screen */
    default: break;
    }
    /* redraw to clear popup */
    apply_layout(c);
}

/* ── Settings menu interactive handler ──────────────────────────────────── */
#define SETTINGS_PAGES 4

static void settings_exit(zt_ctx *c) {
    c->tui.settings_mode = false;
    c->tui.popup_active  = false;
    apply_layout(c);
}

static void settings_page_left(zt_ctx *c) {
    c->tui.settings_page = (c->tui.settings_page + SETTINGS_PAGES - 1) % SETTINGS_PAGES;
    draw_settings_page(c);
}

static void settings_page_right(zt_ctx *c) {
    c->tui.settings_page = (c->tui.settings_page + 1) % SETTINGS_PAGES;
    draw_settings_page(c);
}

/* Per-page key handlers. Return true if the key was consumed. */
static bool settings_handle_serial(zt_ctx *c, unsigned char k) {
    switch (k) {
    case 'b':
    case 'B': { /* cycle common baud rates */
        static const unsigned bauds[] = {
            9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000,
        };
        int cnt = (int)(sizeof bauds / sizeof bauds[0]);
        int cur = 0;
        for (int i = 0; i < cnt; i++)
            if (bauds[i] == c->serial.baud) {
                cur = i;
                break;
            }
        c->serial.baud = bauds[(cur + 1) % cnt];
        if (c->serial.fd >= 0) set_custom_baud(c->serial.fd, c->serial.baud);
        return true;
    }
    case 'c':
    case 'C':
        c->serial.data_bits = c->serial.data_bits == 8   ? 7
                              : c->serial.data_bits == 7 ? 6
                              : c->serial.data_bits == 6 ? 5
                                                         : 8;
        return true;
    case 'd':
    case 'D':
        c->serial.parity = c->serial.parity == 'n' ? 'e' : c->serial.parity == 'e' ? 'o' : 'n';
        return true;
    case 'e':
    case 'E': c->serial.stop_bits = c->serial.stop_bits == 1 ? 2 : 1; return true;
    case 'f':
    case 'F':
        c->serial.flow = (c->serial.flow + 1) % 3;
        if (c->serial.fd >= 0) apply_flow(c->serial.fd, c->serial.flow);
        return true;
    case 'g':
    case 'G': c->proto.mode = (c->proto.mode + 1) % ZT_FRAME__COUNT; return true;
    case 'h':
    case 'H': c->proto.crc_mode = (c->proto.crc_mode + 1) % ZT_CRC__COUNT; return true;
    default: return false;
    }
}

static bool settings_handle_screen(zt_ctx *c, unsigned char k) {
    switch (k) {
    case 'a':
    case 'A': c->proto.color_on = !c->proto.color_on; return true;
    case 'b':
    case 'B': c->proto.local_echo = !c->proto.local_echo; return true;
    case 'c':
    case 'C':
        c->log.hex_mode = !c->log.hex_mode;
        if (c->log.hex_mode) {
            if (c->log.line_len) flush_line(c);
            c->log.hex_col     = 0;
            c->log.hex_row_len = 0;
            c->log.hex_offset  = 0;
        } else {
            hex_flush_row(c);
        }
        return true;
    case 'd':
    case 'D':
        c->proto.show_ts = !c->proto.show_ts;
        c->tui.sb_redraw = true;
        return true;
    case 'e':
    case 'E': c->proto.sgr_passthrough = !c->proto.sgr_passthrough; return true;
    case 'f':
    case 'F':
        if (c->proto.passthrough)
            passthrough_exit(c);
        else
            passthrough_enter(c);
        return true;
    case 'g':
    case 'G': c->log.mute_dbg = !c->log.mute_dbg; return true;
    case 'h':
    case 'H': c->log.mute_inf = !c->log.mute_inf; return true;
    default: return false;
    }
}

static bool settings_handle_kbd(zt_ctx *c, unsigned char k) {
    switch (k) {
    case 'b':
    case 'B':
        c->tui.mouse_tracking = !c->tui.mouse_tracking;
        (void)zt_write_cstr(STDOUT_FILENO, c->tui.mouse_tracking
                                               ? "\033[?1000h\033[?1002h\033[?1006h"
                                               : "\033[?1000l\033[?1002l\033[?1006l");
        return true;
    case 'c':
    case 'C': c->log.watch_beep = !c->log.watch_beep; return true;
    case 'd':
    case 'D': c->proto.osc52_enabled = !c->proto.osc52_enabled; return true;
    case 'e':
    case 'E': c->proto.hyperlinks = !c->proto.hyperlinks; return true;
    case 'f':
    case 'F': c->core.paused = !c->core.paused; return true;
    case 'g':
    case 'G': c->core.reconnect = !c->core.reconnect; return true;
    default: return false;
    }
}

static bool settings_handle_logging(zt_ctx *c, unsigned char k) {
    switch (k) {
    case 'c':
    case 'C': c->log.format = (c->log.format + 1) % ZT_LOG__COUNT; return true;
    case 'e':
    case 'E': c->proto.tx_ts = !c->proto.tx_ts; return true;
    default: return false;
    }
}

void handle_settings_key(zt_ctx *c, const unsigned char *buf, size_t n) {
    /* Esc or q → exit settings */
    if (n == 1 && (buf[0] == 0x1B || buf[0] == 'q' || buf[0] == 'Q')) {
        settings_exit(c);
        return;
    }

    /* Arrow keys: Left/Right = page nav, Up/Down = ignored */
    if (n >= 3 && buf[0] == 0x1B && buf[1] == '[') {
        if (buf[2] == 'D') {
            settings_page_left(c);
            return;
        }
        if (buf[2] == 'C') {
            settings_page_right(c);
            return;
        }
        return; /* ignore up/down */
    }

    /* Tab = next page, Shift-Tab (if detected) = prev page */
    if (n == 1 && buf[0] == '\t') {
        settings_page_right(c);
        return;
    }

    /* Per-page key handling */
    if (n == 1) {
        unsigned char k        = buf[0];
        int           pg       = c->tui.settings_page % SETTINGS_PAGES;
        bool          consumed = false;
        switch (pg) {
        case 0: consumed = settings_handle_serial(c, k); break;
        case 1: consumed = settings_handle_screen(c, k); break;
        case 2: consumed = settings_handle_kbd(c, k); break;
        case 3: consumed = settings_handle_logging(c, k); break;
        }
        if (consumed) {
            draw_settings_page(c); /* redraw to reflect change */
            return;
        }
    }
    /* Unknown key — ignore (stay in settings) */
}

void handle_escape_seq(zt_ctx *c, const unsigned char *buf, size_t n) {
    /* F1..F12 macro lookup takes precedence */
    {
        int fk = fkey_index(buf, n);
        if (fk > 0) {
            const char *m = c->ext.macros[fk - 1];
            if (m && *m) {
                macro_fire(c, fk);
                return;
            }
            /* no macro bound: drop silently (don't forward raw F-key) */
            return;
        }
    }

    /* PgUp / PgDn — always intercept for scrollback */
    if (n >= 4 && buf[0] == 0x1B && buf[1] == '[' && buf[3] == '~') {
        int vis = c->tui.rows - 2;
        if (vis < 1) vis = 1;
        if (buf[2] == '5') {
            scroll_up(c, vis);
            return;
        }
        if (buf[2] == '6') {
            scroll_down(c, vis);
            return;
        }
    }
    /* Shift+PgUp / Shift+PgDn: \033[5;2~ / \033[6;2~ */
    if (n >= 6 && buf[0] == 0x1B && buf[1] == '[' && buf[3] == ';' && buf[4] == '2' &&
        buf[5] == '~') {
        int vis = c->tui.rows - 2;
        if (vis < 1) vis = 1;
        if (buf[2] == '5') {
            scroll_up(c, vis);
            return;
        }
        if (buf[2] == '6') {
            scroll_down(c, vis);
            return;
        }
    }

    /* SGR mouse: \033[<btn;x;yM  (press / drag)
     *            \033[<btn;x;ym  (release)
     * btn:  0..2  = left/middle/right press
     *       +4    = Shift held
     *       +32   = motion while button held (drag)
     *       64/65 = wheel up/down
     */
    if (n >= 6 && buf[1] == '[' && buf[2] == '<') {
        unsigned btn = 0;
        int      x = 0, y = 0;
        int      pos = 3;
        while (pos < (int)n && buf[pos] >= '0' && buf[pos] <= '9')
            btn = btn * 10 + (buf[pos++] - '0');
        if (pos < (int)n && buf[pos] == ';') pos++;
        while (pos < (int)n && buf[pos] >= '0' && buf[pos] <= '9')
            x = x * 10 + (buf[pos++] - '0');
        if (pos < (int)n && buf[pos] == ';') pos++;
        while (pos < (int)n && buf[pos] >= '0' && buf[pos] <= '9')
            y = y * 10 + (buf[pos++] - '0');
        while (pos < (int)n && buf[pos] != 'M' && buf[pos] != 'm')
            pos++;
        if (pos >= (int)n) return;
        char kind = (char)buf[pos]; /* 'M' press/drag, 'm' release */

        /* wheel */
        if (btn == 64 || btn == 65) {
            if (btn == 64)
                scroll_up(c, 3);
            else
                scroll_down(c, 3);
            return;
        }

        /* scrollbar drag: left button on rightmost column starts, drag updates,
         * release ends. Motion events have the +32 bit set. */
        bool     is_motion = (btn & 32) != 0;
        unsigned base      = btn & ~32u;
        if (base == 0) { /* left button */
            int vis = c->tui.rows - 2;
            if (vis < 1) vis = 1;
            int maxoff = c->log.sb_count - vis;
            if (maxoff < 0) maxoff = 0;
            /* y is 1-based; scroll region starts at row 2 */
            int row_in_vis = y - 2;
            if (row_in_vis < 0) row_in_vis = 0;
            if (row_in_vis >= vis) row_in_vis = vis - 1;
            if (kind == 'M' && !is_motion) {
                /* press */
                if (x >= c->tui.cols && maxoff > 0) {
                    /* press on right-edge scrollbar \u2192 jump-scroll */
                    c->tui.sb_dragging = true;
                    int new_top        = row_in_vis * maxoff / (vis > 1 ? vis - 1 : 1);
                    int target         = maxoff - new_top;
                    if (target < 0) target = 0;
                    if (target > maxoff) target = maxoff;
                    int delta = target - c->tui.sb_offset;
                    if (delta > 0)
                        scroll_up(c, delta);
                    else if (delta < 0)
                        scroll_down(c, -delta);
                } else if (y >= 2 && y < c->tui.rows && x >= 1 && x < c->tui.cols) {
                    /* press inside the body region \u2192 start text selection.
                     * Anchor and end coincide; sel_active stays false until
                     * the user actually drags so a plain click clears any
                     * previous selection without showing a 1-cell highlight. */
                    selection_begin(c, y, x);
                }
                return;
            }
            if (kind == 'M' && is_motion) {
                if (c->tui.sb_dragging) {
                    if (maxoff > 0) {
                        int new_top = row_in_vis * maxoff / (vis > 1 ? vis - 1 : 1);
                        int target  = maxoff - new_top;
                        if (target < 0) target = 0;
                        if (target > maxoff) target = maxoff;
                        int delta = target - c->tui.sb_offset;
                        if (delta > 0)
                            scroll_up(c, delta);
                        else if (delta < 0)
                            scroll_down(c, -delta);
                    }
                    return;
                }
                if (c->tui.sel_dragging) {
                    /* extend selection. Clamp into body region so dragging
                     * off the top/bottom edge still gives a sane endpoint. */
                    int yy = y, xx = x;
                    if (yy < 2) yy = 2;
                    if (yy >= c->tui.rows) yy = c->tui.rows - 1;
                    if (xx < 1) xx = 1;
                    if (xx >= c->tui.cols) xx = c->tui.cols - 1;
                    selection_extend(c, yy, xx);
                    return;
                }
            }
            if (kind == 'm') {
                if (c->tui.sb_dragging) {
                    c->tui.sb_dragging = false;
                    return;
                }
                if (c->tui.sel_dragging) {
                    selection_finish(c);
                    return;
                }
            }
        }
        if (base == 2) { /* right button */
            if (kind == 'M' && !is_motion && c->tui.sel_active) {
                /* right-click on existing selection \u2192 re-copy. Useful when
                 * the host terminal silently dropped the first OSC 52 push
                 * (e.g. clipboard write disabled in terminal settings) and
                 * the user wants to retry without re-dragging. */
                selection_copy(c);
                return;
            }
        }
        return; /* ignore other button events */
    }

    /* X10 mouse: \033[M + 3 raw bytes (wheel only) */
    if (n >= 6 && buf[1] == '[' && buf[2] == 'M') {
        if (buf[3] == 0x60) {
            scroll_up(c, 3);
            return;
        }
        if (buf[3] == 0x61) {
            scroll_down(c, 3);
            return;
        }
        return;
    }

    /* In scroll mode: arrows scroll line-by-line, other keys exit */
    if (c->tui.sb_offset > 0) {
        if (n >= 3 && buf[1] == '[') {
            if (buf[2] == 'A') {
                scroll_up(c, 1);
                return;
            }
            if (buf[2] == 'B') {
                scroll_down(c, 1);
                return;
            }
        }
        leave_scroll(c);
    }

    if (c->tui.sent_len == 0 && n >= 3 && buf[0] == 0x1B && buf[1] == '[') {
        switch (buf[2]) {
        case 'A':
            if (c->tui.hist_count) {
                if (c->tui.hist_view < c->tui.hist_count) c->tui.hist_view++;
                load_history_into_buf(c);
                draw_input(c);
            }
            return;
        case 'B':
            if (c->tui.hist_view > 0) {
                c->tui.hist_view--;
                if (c->tui.hist_view == 0) {
                    c->tui.input_len = 0;
                    c->tui.sent_len  = 0;
                    c->tui.cursor    = 0;
                } else
                    load_history_into_buf(c);
                draw_input(c);
            }
            return;
        case 'C':
            if (c->tui.cursor + c->tui.sent_len < c->tui.input_len) c->tui.cursor++;
            draw_input(c);
            return;
        case 'D':
            if (c->tui.cursor > 0) c->tui.cursor--;
            draw_input(c);
            return;
        case 'H':
            c->tui.cursor = 0;
            draw_input(c);
            return;
        case 'F':
            c->tui.cursor = c->tui.input_len - c->tui.sent_len;
            draw_input(c);
            return;
        default: break;
        }
    }
    flush_unsent(c);
    direct_send(c, buf, n);
    c->tui.input_len = 0;
    c->tui.sent_len  = 0;
    c->tui.cursor    = 0;
    draw_input(c);
}

void insert_char(zt_ctx *c, unsigned char k) {
    if (c->tui.input_len >= ZT_INPUT_CAP - 1) return;
    size_t abs = c->tui.sent_len + c->tui.cursor;
    if (abs < c->tui.input_len)
        memmove(&c->tui.input_buf[abs + 1], &c->tui.input_buf[abs], c->tui.input_len - abs);
    c->tui.input_buf[abs] = k;
    c->tui.input_len++;
    c->tui.cursor++;
}

void delete_before_cursor(zt_ctx *c) {
    if (c->tui.cursor > 0) {
        size_t abs = c->tui.sent_len + c->tui.cursor;
        memmove(&c->tui.input_buf[abs - 1], &c->tui.input_buf[abs], c->tui.input_len - abs);
        c->tui.input_len--;
        c->tui.cursor--;
    } else if (c->tui.sent_len > 0) {
        unsigned char bs = 0x7F;
        direct_send(c, &bs, 1);
        c->tui.sent_len--;
        if (c->tui.sent_len < c->tui.input_len) {
            memmove(&c->tui.input_buf[c->tui.sent_len], &c->tui.input_buf[c->tui.sent_len + 1],
                    c->tui.input_len - c->tui.sent_len - 1);
            c->tui.input_len--;
        }
    }
}

void handle_stdin_chunk(zt_ctx *c, const unsigned char *buf, size_t n) {
    c->proto.tab_echo = false; /* stop echo capture on any user input */

    /* ── Settings menu keystrokes ──────────────────────────────────── */
    if (c->tui.settings_mode) {
        handle_settings_key(c, buf, n);
        return;
    }

    /* Rename-log keystrokes: route to the rename bar. */
    if (c->tui.rename_mode) {
        for (size_t i = 0; i < n; i++) {
            unsigned char k = buf[i];
            if (k == 0x1B) { /* Esc — cancel */
                c->tui.rename_mode = false;
                c->tui.rename_len  = 0;
                apply_layout(c);
                return;
            }
            if (k == '\r' || k == '\n') { /* commit rename */
                c->tui.rename_buf[c->tui.rename_len] = 0;
                if (c->tui.rename_len == 0 || !c->log.path) {
                    c->tui.rename_mode = false;
                    apply_layout(c);
                    return;
                }
                /* Use a persistent buffer so c->log.path stays valid. */
                static char renamed[PATH_MAX_LEN];
                snprintf(renamed, sizeof renamed, "%s", c->tui.rename_buf);
                if (rename(c->log.path, renamed) == 0) {
                    c->log.path = renamed;
                    set_flash(c, "log renamed \xe2\x86\x92 %s", renamed);
                } else {
                    set_flash(c, "rename failed: %s", strerror(errno));
                }
                c->tui.rename_mode = false;
                c->tui.ui_dirty    = true;
                apply_layout(c);
                return;
            }
            if (k == 0x7F || k == 0x08) {
                if (c->tui.rename_len) c->tui.rename_buf[--c->tui.rename_len] = 0;
            } else if (k >= 0x20 && k < 0x7F && c->tui.rename_len + 1 < PATH_MAX_LEN) {
                c->tui.rename_buf[c->tui.rename_len++] = (char)k;
            }
        }
        draw_rename_bar(c);
        return;
    }

    /* Search-mode keystrokes: route to search bar, not input/command. */
    if (c->tui.search_mode) {
        for (size_t i = 0; i < n; i++) {
            unsigned char k = buf[i];
            if (k == 0x1B) { /* Esc — cancel */
                c->tui.search_mode = false;
                c->tui.search_len  = 0;
                apply_layout(c);
                return;
            }
            if (k == '\r' || k == '\n') { /* run search */
                c->tui.search_buf[c->tui.search_len] = 0;
                if (c->tui.search_len == 0) {
                    c->tui.search_mode = false;
                    apply_layout(c);
                    return;
                }
                c->tui.search_current = 0;
                if (search_scrollback(c, +1)) {
                    c->tui.search_current = 1;
                    redraw_scrollback(c);
                    set_flash(c, "match #%d for \"%s\"", c->tui.search_current,
                              c->tui.search_buf);
                } else {
                    set_flash(c, "no match for \"%s\"", c->tui.search_buf);
                }
                c->tui.search_mode = false;
                c->tui.ui_dirty    = true;
                apply_layout(c);
                return;
            }
            if (k == 0x7F || k == 0x08) {
                if (c->tui.search_len) c->tui.search_buf[--c->tui.search_len] = 0;
            } else if (k >= 0x20 && k < 0x7F && c->tui.search_len + 1 < ZT_SEARCH_CAP) {
                c->tui.search_buf[c->tui.search_len++] = (char)k;
            }
        }
        draw_search_bar(c);
        return;
    }

    if (n > 1 && buf[0] == 0x1B) {
        handle_escape_seq(c, buf, n);
        return;
    }

    for (size_t i = 0; i < n; i++) {
        unsigned char k = buf[i];
        if (c->tui.command_mode) {
            handle_cmd_key(c, k);
            continue;
        }

        /* In scroll mode with an active search query, n/N cycle hits. */
        if (c->tui.sb_offset > 0 && c->tui.search_len > 0 && (k == 'n' || k == 'N')) {
            int dir = (k == 'n') ? +1 : -1;
            if (search_scrollback(c, dir)) {
                redraw_scrollback(c);
                c->tui.search_current += dir;
                if (c->tui.search_current < 1) c->tui.search_current = 1;
                set_flash(c, "match #%d for \"%s\"", c->tui.search_current, c->tui.search_buf);
            } else {
                set_flash(c, "no more matches");
            }
            c->tui.ui_dirty = true;
            continue;
        }

        /* exit scroll mode on any typing */
        leave_scroll(c);

        if (k == 0x01) {
            c->tui.command_mode = true;
            draw_cmd_popup(c);
            continue;
        }
        if (k == 0x1B) {
            c->tui.input_len = 0;
            c->tui.sent_len  = 0;
            c->tui.cursor    = 0;
            draw_input(c);
            continue;
        }
        if (k == '\r' || k == '\n') {
            if (c->tui.input_len > 0) history_push(c, c->tui.input_buf, c->tui.input_len);
            c->tui.hist_view = 0;
            flush_unsent(c);
            unsigned char nl = '\r';
            trickle_send(c, &nl, 1);
            c->tui.input_len = 0;
            c->tui.sent_len  = 0;
            c->tui.cursor    = 0;
            draw_input(c);
            continue;
        }
        if (k == '\t') {
            size_t unsent = c->tui.input_len - c->tui.sent_len;
            flush_unsent(c);
            unsigned char tab = '\t';
            direct_send(c, &tab, 1);
            /* capture echo: skip N chars (echo of what we just flushed),
             * then append completion suffix to buffer */
            c->proto.tab_echo = true;
            c->proto.tab_skip = unsent;
            draw_input(c);
            continue;
        }
        if (k == 0x7F || k == 0x08) {
            delete_before_cursor(c);
            draw_input(c);
            continue;
        }
        if (k == 0x15) {
            if (c->tui.sent_len == 0) {
                c->tui.input_len = 0;
                c->tui.cursor    = 0;
            } else {
                for (size_t j = 0; j < c->tui.sent_len; j++) {
                    unsigned char bs = 0x7F;
                    direct_send(c, &bs, 1);
                }
                c->tui.input_len = 0;
                c->tui.sent_len  = 0;
                c->tui.cursor    = 0;
            }
            draw_input(c);
            continue;
        }
        if (k == 0x17) {
            while (c->tui.cursor > 0 &&
                   isspace(c->tui.input_buf[c->tui.sent_len + c->tui.cursor - 1]))
                delete_before_cursor(c);
            while (c->tui.cursor > 0 &&
                   !isspace(c->tui.input_buf[c->tui.sent_len + c->tui.cursor - 1]))
                delete_before_cursor(c);
            draw_input(c);
            continue;
        }
        if (k == 0x0C) {
            apply_layout(c);
            continue;
        }
        if (k == 0x03) {
            unsigned char etx = 0x03;
            direct_send(c, &etx, 1);
            c->tui.input_len = 0;
            c->tui.sent_len  = 0;
            c->tui.cursor    = 0;
            draw_input(c);
            continue;
        }

        insert_char(c, k);
        if (c->proto.local_echo) {
            direct_send(c, &k, 1);
            c->tui.sent_len = c->tui.input_len;
            c->tui.cursor   = 0;
        }
        draw_input(c);
    }
}
