/**
 * @file reconnect.c
 * @brief Reconnect: probe + disconnect dialog + wait loop
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

/* ------------------------------ reconnect -------------------------------- */

/* Best-effort reopen: single attempt (run_interactive retries with poll timeout).
 *
 * PRECONDITION: callers must ensure the RX worker (if any) is suspended
 * before calling — installing a new c->serial.fd here would otherwise
 * race the worker's read on its dup of the old one. The wait loop in
 * run_reconnect_loop() and the 'r' handler in input.c both take care
 * of suspending around their call. */
int reconnect_attempt(zt_ctx *c) {
    /* If --port-glob / --match-vid-pid were given, re-resolve the device
     * path each attempt. A USB-serial adapter that comes back as a
     * different /dev/ttyUSBn after replug is then transparently picked
     * up; without these hints the device path is held fixed. */
    (void)port_rediscover(c);

    int fd = try_reopen_serial(c->serial.device, c->serial.baud, c->serial.data_bits,
                               c->serial.parity, c->serial.stop_bits, c->serial.flow);
    if (fd < 0) return -1;
    c->serial.fd = fd;
    return 0;
}

/* Forward decls */

/* Centered modal shown while the device is disconnected. Amber border, animated
 * dots, and a footer hint. Redrawn each tick of the reconnect loop. */
void draw_disconnect_popup(zt_ctx *c, int dots) {
    char        line_wait[128];
    const char *dotstr  = "...";
    char        anim[5] = {0};
    for (int i = 0; i < dots && i < 3; i++)
        anim[i] = '.';
    for (int i = dots; i < 3; i++)
        anim[i] = ' ';
    (void)dotstr;
    snprintf(line_wait, sizeof line_wait,
             "\033[38;5;252mwaiting for "
             "\033[1;38;5;220m%s\033[0;38;5;252m"
             "\033[1;38;5;208m%s\033[0m",
             c->serial.device, anim);

    char line_baud[96];
    snprintf(line_baud, sizeof line_baud, "\033[2;38;5;245m%u baud \xc2\xb7 %d%c%d\033[0m",
             c->serial.baud, c->serial.data_bits,
             c->serial.parity == 0 ? 'N' : (c->serial.parity == 1 ? 'O' : 'E'),
             c->serial.stop_bits);

    const char *body[] = {
        "",        "\033[38;5;208m\xe2\x97\x8f\033[0m \033[1;97mdevice link lost\033[0m",
        "",        line_wait,
        line_baud, "",
    };
    draw_dialog(c, "\xe2\x9a\xa0",                          /* ⚠ */
                "connection interrupted", "\033[38;5;208m", /* amber accent */
                body, (int)(sizeof body / sizeof body[0]),
                "Ctrl+A x to quit \xc2\xb7 Ctrl+A r to force retry");
    ob_flush();
}

/* Wait-and-retry reconnect loop. Keeps the UI responsive while polling the
 * device with ZT_RECONNECT_MS granularity. Returns when reconnected, or
 * when the user quits.
 *
 * UX contract while disconnected:
 *   • PgUp / PgDn / arrow scroll the scrollback as normal. The modal popup
 *     auto-hides as soon as sb_offset > 0 so the user can read history
 *     they were monitoring before the unplug. The HUD shows a persistent
 *     "◆ DISCONNECTED" pill so the state is never lost — see draw_hud().
 *   • Mouse drag → in-app selection → OSC 52 copy works normally.
 *   • Ctrl+A / starts a scrollback search; n / N cycle hits.
 *   • Ctrl+A r force-retries; Ctrl+A x / q quits. All other Ctrl+A keys
 *     are no-ops (gated by c->tui.disconnected in handle_cmd_key) so a
 *     stray keystroke can't spawn a log file or toggle modes that don't
 *     make sense without a device.
 *   • Returning to bottom (sb_offset == 0) brings the modal back. */
void run_reconnect_loop(zt_ctx *c) {
    /* Stop the RX worker for the whole disconnect window — its dup of
     * the old fd would otherwise keep returning EIO and spinning. */
    rx_thread_pause(c);
    if (c->serial.fd >= 0) {
        close(c->serial.fd);
        c->serial.fd = -1;
    }
    hooks_on_event(c, ZT_HOOK_EVENT_DISCONNECT);
    /* Dismiss command mode + scrollbar drag; we will own the popup state. */
    c->tui.command_mode = false;
    c->tui.sb_dragging  = false;
    c->tui.disconnected = true;
    apply_layout(c);
    c->tui.popup_active = false;
    draw_hud(c);
    draw_input(c);
    c->tui.ui_dirty = false;

    unsigned char rbuf[ZT_READ_CHUNK];
    int           dots          = 0;
    bool          popup_visible = false;
    /* If the user wasn't already scrolling, drop the modal in. */
    if (c->tui.sb_offset == 0) {
        draw_disconnect_popup(c, dots);
        c->tui.popup_active = true;
        popup_visible       = true;
    }
    ob_flush();

    while (!zt_g_quit) {
        if (zt_g_winch) {
            zt_g_winch = 0;
            query_winsize(c);
            apply_layout(c);
            c->tui.popup_active = false;
            draw_hud(c);
            draw_input(c);
            popup_visible = false; /* force re-draw of popup below if needed */
        }
        struct pollfd sp = {.fd = STDIN_FILENO, .events = POLLIN};
        int           pr = poll(&sp, 1, ZT_RECONNECT_MS);
        if (pr > 0 && (sp.revents & POLLIN)) {
            for (;;) {
                ssize_t r = read(STDIN_FILENO, rbuf, sizeof rbuf);
                if (r > 0) {
                    handle_stdin_chunk(c, rbuf, (size_t)r);
                    /* stdin is blocking (raw mode VMIN=1); short read means
                     * the kernel buffer is drained — looping would block
                     * the reconnect tick forever, leaving Ctrl+A x stuck. */
                    if ((size_t)r < sizeof rbuf) break;
                    continue;
                }
                if (r == 0) {
                    zt_g_quit = 1;
                    break;
                }
                if (errno == EINTR) continue;
                break;
            }
            if (zt_g_quit) break;
        }

        /* Repaint logic. Three cases:
         *   1. User is scrolling (sb_offset > 0) → hide modal, repaint
         *      scrollback + HUD when dirty. No popup tick.
         *   2. User just scrolled back to live (sb_offset == 0 but popup
         *      not visible yet) → fully repaint underlay (scrollback +
         *      HUD + input), then draw the modal fresh.
         *   3. Live view, modal already up → tick the spinner. */
        if (c->tui.sb_offset > 0) {
            if (c->tui.sb_redraw || c->tui.ui_dirty) {
                redraw_scrollback(c);
                draw_hud(c);
                draw_input(c);
                c->tui.sb_redraw = false;
                c->tui.ui_dirty  = false;
            }
            c->tui.popup_active = false;
            popup_visible       = false;
        } else {
            if (!popup_visible || c->tui.sb_redraw || c->tui.ui_dirty) {
                /* Underlay first (clears any prior popup pixels), then
                 * popup on top. */
                redraw_scrollback(c);
                draw_hud(c);
                draw_input(c);
                c->tui.sb_redraw = false;
                c->tui.ui_dirty  = false;
            }
            dots = (dots + 1) % 4;
            draw_disconnect_popup(c, dots);
            c->tui.popup_active = true;
            popup_visible       = true;
        }
        ob_flush();

        if (reconnect_attempt(c) == 0) {
            /* Restart the RX worker (if --threaded) now that
             * c->serial.fd is valid again — it'll dup the new fd. */
            rx_thread_unpause(c);
            c->tui.popup_active = false;
            c->tui.disconnected = false;
            /* Don't yank the view out from under a user mid-scroll —
             * just flash that we're back and let them PgDn to live
             * when they're ready. */
            apply_layout(c);
            set_flash(c, "\xe2\x9c\x93 reconnected");
            c->tui.ui_dirty = true;
            hooks_on_event(c, ZT_HOOK_EVENT_CONNECT);
            return;
        }
    }
    c->tui.popup_active = false;
    c->tui.disconnected = false;
    /* User quit mid-disconnect: leave the worker stopped — main is on
     * its way to teardown anyway and the fd is still -1. */
}
