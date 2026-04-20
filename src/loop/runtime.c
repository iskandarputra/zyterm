/**
 * @file runtime.c
 * @brief run_interactive / run_dump / run_replay top loops
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

/* ------------------------------ run loop --------------------------------- */

int run_interactive(zt_ctx *c) {
    setup_stdin_raw();
    /* Register restore_terminal at most once per process — ATEXIT_MAX is
     * typically 32, and when embedded in zy we'd exhaust it quickly. */
    static bool s_atexit_registered = false;
    if (!s_atexit_registered) {
        atexit(restore_terminal);
        s_atexit_registered = true;
    }
    query_winsize(c);

    /* alt screen + SGR mouse tracking + cursor on (managed once here,
     * never per frame — see draw_hud()/draw_input() comments).
     *   1000 = button events (press/release + wheel)
     *   1002 = also report motion while a button is held (drag)
     *   1006 = SGR extended coords (reliable beyond col 223)
     *   ?25h cursor visible
     *   ?12l stop the cursor from blinking (AT&T 610). On terminals
     *        that ignore ?12l (rare), the DECSCUSR "2 q" below also
     *        forces a steady block. Without these the input-bar caret
     *        flickers at the terminal's default 1–2 Hz blink rate
     *        which the user perceives as UI jitter on every keystroke.
     *   2 q  DECSCUSR steady block style.
     * Mouse tracking is ON by default. zyterm captures every click,
     * drag and wheel event so we can offer:
     *   • wheel-up/down              → scroll scrollback
     *   • left click + drag (body)   → in-app text selection (cells
     *                                   highlighted in reverse video,
     *                                   text copied to system clipboard
     *                                   via OSC 52 on release)
     *   • right click on selection   → re-copy to clipboard
     *   • left drag on right column  → scrollbar jump
     * This is the only way to have wheel scroll AND mouse-driven copy
     * in the same session: with the host terminal owning the mouse, the
     * wheel scrolls the host's (empty alt-screen) scrollback instead of
     * ours. Toggle OFF with Ctrl+A m to fall back to native host-terminal
     * selection (no app-level wheel/scrollbar drag, no OSC 52 copy). */
    (void)zt_write_cstr(STDOUT_FILENO, "\033[?1049h\033[?1000h\033[?1002h\033[?1006h"
                                       "\033[?25h\033[?12l\033[2 q");
    zt_g_ui_active        = true;
    c->tui.mouse_tracking = true;
    apply_layout(c);

    (void)zt_write_cstr(STDOUT_FILENO, "\0338");
    char banner[512];
    int  bn = snprintf(banner, sizeof banner,
                       "\033[38;5;60m\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\033[0m "
                        "\033[1;38;5;111mzyterm " ZT_VERSION "\033[0m "
                        "\033[38;5;60m\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\033[0m\r\n"
                        "    \033[38;5;114m\xe2\x97\x8f\033[0m "
                        "\033[38;5;252mconnected to "
                        "\033[1;38;5;183m%s\033[0;38;5;252m @ "
                        "\033[1;38;5;80m%u\033[0;38;5;252m baud\033[0m\r\n"
                        "    \033[38;5;60m\xe2\x94\x94\033[0m "
                        "\033[38;5;245mCtrl+A \xe2\x86\x92 menu  "
                        "\xc2\xb7  Ctrl+A ? \xe2\x86\x92 help  "
                        "\xc2\xb7  drag \xe2\x86\x92 select+copy\033[0m\r\n",
                       c->serial.device, c->serial.baud);
    if (bn > 0) (void)zt_write_all(STDOUT_FILENO, banner, (size_t)bn);
    (void)zt_write_cstr(STDOUT_FILENO, "\0337");
    draw_hud(c);
    draw_input(c);
    ob_flush();

    now(&c->core.t_start);
    c->core.t_last_hud   = c->core.t_start;
    c->core.t_last_paint = c->core.t_start;
    c->tui.ui_dirty      = false;

    unsigned char rbuf[ZT_READ_CHUNK];
    struct pollfd pfds[2];

    while (!zt_g_quit) {
        if (zt_g_winch) {
            zt_g_winch = 0;
            query_winsize(c);
            apply_layout(c);
        }

        pfds[0].fd      = c->serial.fd;
        pfds[0].events  = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd      = STDIN_FILENO;
        pfds[1].events  = POLLIN;
        pfds[1].revents = 0;

        int pr          = poll(pfds, 2, ZT_HUD_REFRESH_MS);
        if (pr < 0) {
            if (errno == EINTR) continue;
            zt_warn("zyterm: poll: %s", strerror(errno));
            break;
        }
        if (pr == 0) {
            c->tui.ui_dirty = true;
        }

        if (pfds[0].revents & (POLLERR | POLLNVAL)) {
            if (c->core.reconnect) {
                run_reconnect_loop(c);
                if (!zt_g_quit) continue;
            }
            zt_warn("zyterm: serial device error");
            break;
        }
        if (pfds[0].revents & POLLHUP) {
            ssize_t r = read(c->serial.fd, rbuf, sizeof rbuf);
            if (r > 0) {
                log_write(c, rbuf, (size_t)r);
                rx_ingest(c, rbuf, (size_t)r);
            }
            if (c->core.reconnect) {
                run_reconnect_loop(c);
                if (!zt_g_quit) continue;
            }
            zt_warn("zyterm: serial hung up");
            break;
        }
        if (pfds[0].revents & POLLIN) {
            for (;;) {
                ssize_t r = read(c->serial.fd, rbuf, sizeof rbuf);
                if (r > 0) {
                    log_write(c, rbuf, (size_t)r);
                    rx_ingest(c, rbuf, (size_t)r);
                    if ((size_t)r < sizeof rbuf) break;
                    continue;
                }
                if (r == 0) break;
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (c->core.reconnect &&
                    (errno == EIO || errno == ENXIO || errno == ENODEV || errno == EBADF)) {
                    run_reconnect_loop(c);
                    break;
                }
                zt_warn("zyterm: read(serial): %s", strerror(errno));
                zt_g_quit = 1;
                break;
            }
        }
        if (pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            zt_g_quit       = 1;
            c->tui.ui_dirty = true;
            continue;
        }
        if (pfds[1].revents & POLLIN) {
            for (;;) {
                ssize_t r = read(STDIN_FILENO, rbuf, sizeof rbuf);
                if (r > 0) {
                    handle_stdin_chunk(c, rbuf, (size_t)r);
                    if ((size_t)r < sizeof rbuf) break;
                    continue;
                }
                if (r == 0) {
                    zt_g_quit = 1;
                    break;
                }
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                zt_warn("zyterm: read(stdin): %s", strerror(errno));
                zt_g_quit = 1;
                break;
            }
            c->tui.ui_dirty = true;
        }

        /* Single draw + flush per iteration (skip while popup is on top).
         *
         * Frame-rate cap (~60 fps): at high baud rates each RX chunk
         * sets ui_dirty, which used to force a draw_hud + draw_input
         * on every loop iteration. Even with output buffering, the
         * downstream terminal's reflow + repaint cost dominates and
         * the user sees jitter. Coalescing to one paint per ~16 ms
         * makes scrollback feel buttery without affecting RX latency
         * (bytes still hit log/scrollback immediately via render_rx).
         *
         * Tab-completion echo (c->proto.tab_echo) bypasses the cap: the
         * user is actively watching the input bar grow letter-by-
         * letter and a 16 ms stall feels like a stutter. The echo
         * window is short (single completion round-trip) so the
         * extra paints have negligible cost. */
        if (c->tui.ui_dirty && !c->tui.popup_active) {
            struct timespec tnow;
            now(&tnow);
            double since = ts_diff_sec(&tnow, &c->core.t_last_paint);
            if (since >= 0.016 || c->proto.tab_echo) {
                if (c->tui.sb_redraw) {
                    redraw_scrollback(c);
                    if (c->tui.sb_offset == 0) {
                        /* back to live — restore cursor at bottom of scroll region */
                        char mv[32];
                        snprintf(mv, sizeof mv, "\033[%d;1H\0337", c->tui.rows - 1);
                        ob_cstr(mv);
                    }
                    c->tui.sb_redraw = false;
                }
                draw_hud(c);
                draw_input(c);
                c->tui.ui_dirty      = false;
                c->core.t_last_paint = tnow;
            }
        }
        /* ancillary subsystem ticks — cheap no-ops when disabled */
        if (c->ext.filter_stdout_fd >= 0) filter_drain(c);
        if (c->net.http_fd >= 0) http_tick(c);
        if (c->net.metrics_fd >= 0) metrics_tick(c);
        if (c->net.session_fd >= 0) session_tick(c);
        if (c->ext.profile_inotify_fd > 0) profile_watch_tick(c);
        tty_stats_poll(c);
        ob_flush();
    }
    return 0;
}

int run_dump(zt_ctx *c, int seconds) {
    unsigned char rbuf[ZT_READ_CHUNK];
    struct pollfd pfd = {.fd = c->serial.fd, .events = POLLIN};
    now(&c->core.t_start);
    struct timespec deadline = c->core.t_start;
    if (seconds > 0) deadline.tv_sec += seconds;

    while (!zt_g_quit) {
        int timeout_ms = -1;
        if (seconds > 0) {
            struct timespec t;
            now(&t);
            double rem = ts_diff_sec(&deadline, &t);
            if (rem <= 0) break;
            timeout_ms = (int)(rem * 1000.0);
        }
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) break;
        if (pfd.revents & (POLLERR | POLLNVAL | POLLHUP)) {
            ssize_t r = read(c->serial.fd, rbuf, sizeof rbuf);
            if (r > 0) {
                log_write(c, rbuf, (size_t)r);
                cast_record_o(rbuf, (size_t)r);
                (void)zt_write_all(STDOUT_FILENO, rbuf, (size_t)r);
                c->core.rx_bytes += (uint64_t)r;
            }
            break;
        }
        if (pfd.revents & POLLIN) {
            for (;;) {
                ssize_t r = read(c->serial.fd, rbuf, sizeof rbuf);
                if (r > 0) {
                    log_write(c, rbuf, (size_t)r);
                    cast_record_o(rbuf, (size_t)r);
                    (void)zt_write_all(STDOUT_FILENO, rbuf, (size_t)r);
                    c->core.rx_bytes += (uint64_t)r;
                    if ((size_t)r < sizeof rbuf) break;
                    continue;
                }
                if (r == 0) break;
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                return 1;
            }
        }
    }
    struct timespec t;
    now(&t);
    fprintf(stderr, "\nzyterm: captured %llu bytes in %.2fs\n",
            (unsigned long long)c->core.rx_bytes, ts_diff_sec(&t, &c->core.t_start));
    return 0;
}

/* ------------------------------ replay ----------------------------------- */

/* Replay a previously-captured log file as if it came from the device.
 * Interactive UI stays live; serial_fd is not used. */
int run_replay(zt_ctx *c) {
    FILE *f = fopen(c->core.replay_path, "rb");
    if (!f) zt_die("zyterm: open(%s): %s", c->core.replay_path, strerror(errno));

    setup_stdin_raw();
    {
        static bool s_atexit_registered = false;
        if (!s_atexit_registered) {
            atexit(restore_terminal);
            s_atexit_registered = true;
        }
    }
    query_winsize(c);
    /* Mouse capture ON — see run_interactive() for full rationale.
     * Replay mode benefits because the user typically wants to copy
     * snippets of recorded traffic; in-app selection makes that just
     * work without involving the host terminal. */
    (void)zt_write_cstr(STDOUT_FILENO, "\033[?1049h\033[?1000h\033[?1002h\033[?1006h"
                                       "\033[?25h\033[?12l\033[2 q");
    zt_g_ui_active        = true;
    c->tui.mouse_tracking = true;
    apply_layout(c);

    char banner[512];
    int  bn = snprintf(banner, sizeof banner,
                       "\0338\033[38;5;60m\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\033[0m "
                        "\033[1;38;5;111mzyterm " ZT_VERSION "\033[0m "
                        "\033[38;5;235m\033[48;5;220m \033[1;30mREPLAY\033[22m \033[0m "
                        "\033[38;5;252m%s\033[0m\r\n"
                        "    \033[38;5;60m\xe2\x94\x94\033[0m "
                        "\033[38;5;245mCtrl+A x to exit\033[0m\r\n\0337",
                       c->core.replay_path);
    if (bn > 0) (void)zt_write_all(STDOUT_FILENO, banner, (size_t)bn);
    draw_hud(c);
    draw_input(c);
    ob_flush();

    now(&c->core.t_start);
    c->core.t_last_hud   = c->core.t_start;
    c->core.t_last_paint = c->core.t_start;
    /* per-byte delay; speed=1.0 => ~100us (~10kB/s), 0 => no delay */
    long delay_ns = 0;
    if (c->core.replay_speed > 0.0) delay_ns = (long)(100000.0 / c->core.replay_speed);
    struct timespec pause = {0, delay_ns};

    unsigned char   rbuf[4096];
    struct pollfd   pf = {.fd = STDIN_FILENO, .events = POLLIN};

    while (!zt_g_quit) {
        if (zt_g_winch) {
            zt_g_winch = 0;
            query_winsize(c);
            apply_layout(c);
        }

        /* drain stdin without blocking */
        int pr = poll(&pf, 1, 0);
        if (pr > 0 && (pf.revents & POLLIN)) {
            for (;;) {
                ssize_t r = read(STDIN_FILENO, rbuf, sizeof rbuf);
                if (r > 0) {
                    handle_stdin_chunk(c, rbuf, (size_t)r);
                    continue;
                }
                break;
            }
        }

        size_t got = fread(rbuf, 1, sizeof rbuf, f);
        if (got == 0) break;
        log_write(c, rbuf, got);
        render_rx(c, rbuf, got);
        if (c->tui.ui_dirty && !c->tui.popup_active) {
            if (c->tui.sb_redraw) {
                redraw_scrollback(c);
                if (c->tui.sb_offset == 0) {
                    char mv[32];
                    snprintf(mv, sizeof mv, "\033[%d;1H\0337", c->tui.rows - 1);
                    ob_cstr(mv);
                }
                c->tui.sb_redraw = false;
            }
            draw_hud(c);
            draw_input(c);
            c->tui.ui_dirty = false;
        }
        ob_flush();
        if (delay_ns > 0) nanosleep(&pause, NULL);
    }
    fclose(f);
    log_notice(c, "replay complete — press Ctrl+A x to quit");
    /* keep UI alive for inspection */
    while (!zt_g_quit) {
        if (zt_g_winch) {
            zt_g_winch = 0;
            query_winsize(c);
            apply_layout(c);
        }
        int pr = poll(&pf, 1, ZT_HUD_REFRESH_MS);
        if (pr > 0 && (pf.revents & POLLIN)) {
            for (;;) {
                ssize_t r = read(STDIN_FILENO, rbuf, sizeof rbuf);
                if (r > 0) {
                    handle_stdin_chunk(c, rbuf, (size_t)r);
                    continue;
                }
                break;
            }
        }
        if (c->tui.ui_dirty && !c->tui.popup_active) {
            if (c->tui.sb_redraw) {
                redraw_scrollback(c);
                if (c->tui.sb_offset == 0) {
                    char mv[32];
                    snprintf(mv, sizeof mv, "\033[%d;1H\0337", c->tui.rows - 1);
                    ob_cstr(mv);
                }
                c->tui.sb_redraw = false;
            }
            draw_hud(c);
            draw_input(c);
            c->tui.ui_dirty = false;
        }
        ob_flush();
    }
    return 0;
}
