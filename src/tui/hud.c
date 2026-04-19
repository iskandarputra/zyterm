/**
 * @file hud.c
 * @brief HUD + input bar + dialogs + command popup
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

/* ------------------------------ HUD -------------------------------------- */

void fmt_bytes(uint64_t v, char *out, size_t cap) {
    static const char *u[] = {"B", "K", "M", "G", "T"};
    double             d   = (double)v;
    size_t             i   = 0;
    while (d >= 1024.0 && i < 4) {
        d /= 1024.0;
        i++;
    }
    if (i == 0)
        snprintf(out, cap, "%llu%s", (unsigned long long)v, u[i]);
    else
        snprintf(out, cap, "%.1f%s", d, u[i]);
}
void fmt_hms(double sec, char *out, size_t cap) {
    unsigned long s = (unsigned long)sec;
    snprintf(out, cap, "%02lu:%02lu:%02lu", s / 3600UL, (s / 60UL) % 60UL, s % 60UL);
}
void query_winsize(zt_ctx *c) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 3 && ws.ws_col > 0) {
        c->tui.rows = ws.ws_row;
        c->tui.cols = ws.ws_col;
    } else {
        c->tui.rows = 24;
        c->tui.cols = 80;
    }
}

/* Visible column width, skipping CSI/OSC escape runs. Treats every UTF-8
 * code-point as 1 cell — callers MUST only use single-width glyphs (ASCII
 * plus box-drawing, arrows, chevrons from U+2500..U+27BF etc). Emoji
 * presentation glyphs (e.g. ⚡ U+26A1) are NOT safe here. */
int visible_len(const char *s) {
    int n = 0;
    while (*s) {
        unsigned char ch = (unsigned char)*s;
        if (ch == 0x1B) {
            s++;
            if (*s == '[') {
                s++;
                while (*s && !((*s >= '@' && *s <= '~')))
                    s++;
                if (*s) s++;
            } else if (*s)
                s++;
        } else if (ch < 0x80) {
            n++;
            s++;
        } else {
            /* UTF-8 continuation: count lead byte as 1 cell, skip continuations. */
            n++;
            if ((ch & 0xE0) == 0xC0)
                s += 2;
            else if ((ch & 0xF0) == 0xE0)
                s += 3;
            else if ((ch & 0xF8) == 0xF0)
                s += 4;
            else
                s++;
        }
    }
    return n;
}

void draw_hud(zt_ctx *c) {
    struct timespec t;
    now(&t);
    double dt = ts_diff_sec(&t, &c->core.t_last_hud);
    if (dt >= 0.25) {
        uint64_t d           = c->core.rx_bytes - c->tui.rx_bytes_last;
        double   inst        = (double)d / dt;
        c->tui.rx_bps        = c->tui.rx_bps * 0.6 + inst * 0.4;
        c->tui.rx_bytes_last = c->core.rx_bytes;
        c->core.t_last_hud   = t;
    }
    double elapsed = ts_diff_sec(&t, &c->core.t_start);
    char   rxs[16], txs[16], bps[16], up[32];
    fmt_bytes(c->core.rx_bytes, rxs, sizeof rxs);
    fmt_bytes(c->core.tx_bytes, txs, sizeof txs);
    fmt_bytes((uint64_t)c->tui.rx_bps, bps, sizeof bps);
    fmt_hms(elapsed, up, sizeof up);

    /* Activity LED: neon when bytes flowing, soft when slow, hollow idle. */
    const char *led = (c->tui.rx_bps > 256.0)  ? "\033[38;5;82m\xe2\x97\x8f"   /* ● neon  */
                      : (c->tui.rx_bps > 16.0) ? "\033[38;5;114m\xe2\x97\x8f"  /* ● soft  */
                                               : "\033[38;5;240m\xe2\x97\x8b"; /* ○ dim   */

    /* Neon cyber palette (all single-cell glyphs):
     *   bg 233, separator 238, brand 51 (cyan), text 255/250
     *   rx 114 (green), tx 81 (cyan), dim 240, accent 214 */

    /* ── Left: brand · device · throughput ── */
    char left[512];
    snprintf(left, sizeof left,
             " \033[38;5;51m\xe2\x96\x8e\033[1;38;5;51m ZYTERM\033[22m"
             "  \033[38;5;238m\xe2\x94\x82  "
             "%s  \033[1;38;5;255m%s\033[22m \033[38;5;238m\xc2\xb7\033[38;5;250m %u"
             "  \033[38;5;238m\xe2\x94\x82  "
             "\033[38;5;240m\xe2\x96\xbe\033[38;5;114m %s "
             "\033[38;5;238m(\033[38;5;245m%s/s\033[38;5;238m)"
             "  \033[38;5;240m\xe2\x80\xa2  "
             "\033[38;5;240m\xe2\x96\xb4\033[38;5;81m %s"
             "  \033[38;5;238m\xe2\x94\x82  "
             "\033[38;5;240mLN \033[38;5;250m%llu ",
             led, c->serial.device, c->serial.baud, rxs, bps, txs,
             (unsigned long long)c->core.rx_lines);

    /* ── Mode indicators (neon diamond pills) ──
     * Each pill: ◆ LBL in accent colour. Clean, minimal, neon. */
#define MPILL(col, lbl) "  \033[38;5;" col "m\xe2\x97\x86 " lbl
    char modes[256];
    modes[0]  = 0;
    size_t mo = 0;
    if (c->core.paused)
        mo += (size_t)snprintf(modes + mo, sizeof modes - mo, MPILL("198", "PAUSE"));
    if (c->log.hex_mode)
        mo += (size_t)snprintf(modes + mo, sizeof modes - mo, MPILL("141", "HEX"));
    if (c->proto.local_echo)
        mo += (size_t)snprintf(modes + mo, sizeof modes - mo, MPILL("81", "ECHO"));
    if (c->proto.show_ts)
        mo += (size_t)snprintf(modes + mo, sizeof modes - mo, MPILL("114", "TS"));
    if (c->log.fd >= 0)
        mo += (size_t)snprintf(modes + mo, sizeof modes - mo, MPILL("82", "LOG"));
    if (c->tui.sb_offset > 0) {
        int pct = c->log.sb_count > 0 ? (c->tui.sb_offset * 100 / c->log.sb_count) : 0;
        mo += (size_t)snprintf(modes + mo, sizeof modes - mo,
                               "  \033[38;5;214m\xe2\x96\xb2 SB %d%%", pct);
    }
#undef MPILL

    bool has_flash =
        c->tui.flash[0] &&
        (t.tv_sec < c->tui.flash_until.tv_sec ||
         (t.tv_sec == c->tui.flash_until.tv_sec && t.tv_nsec < c->tui.flash_until.tv_nsec));
    char flashseg[256];
    flashseg[0] = 0;
    if (has_flash)
        snprintf(flashseg, sizeof flashseg, "  \033[38;5;214m\xe2\x96\xb8 %s", c->tui.flash);

    char right[512];
    snprintf(right, sizeof right, "%s%s  \033[38;5;238m\xe2\x94\x82  \033[38;5;245m%s ", modes,
             flashseg, up);

    /* ── Paint HUD row ──
     * NOTE: we do NOT toggle cursor visibility here. The cursor was
     * hidden once at alt-screen entry and is shown again only at exit.
     * Toggling it on every frame causes visible blink on real terminals
     * (xterm, foot, kitty all show the cursor sprite for ~16 ms each
     * time it transitions). Mid-frame moves with \e[H are silent. */
    ob_cstr("\033[1;1H\033[48;5;233m\033[38;5;250m\033[2K");
    ob_cstr(left);

    int rvis = visible_len(right);
    int col  = c->tui.cols - rvis;
    if (col < 1) col = 1;
    char mv[32];
    int  mvn = snprintf(mv, sizeof mv, "\033[1;%dH\033[48;5;233m", col);
    if (mvn > 0) ob_write(mv, (size_t)mvn);
    ob_cstr(right);
    ob_cstr("\033[0m");
}

/* ------------------------------ input bar -------------------------------- */

/* Build the input prompt. All glyphs are single-cell (no emoji presentation)
 * so visible_len() — which counts every code-point as 1 — returns the exact
 * display column width used for cursor placement. */
size_t build_prompt(zt_ctx *c, char *buf, size_t cap) {
    if (c->tui.command_mode) {
        /* amber accent bar + CMD pill */
        snprintf(buf, cap,
                 "\033[38;5;214m\xe2\x96\x8e"             /* ▎ bar */
                 "\033[48;5;214m\033[1;30m CMD "          /* " CMD " chip */
                 "\033[0;49m "                            /* back to normal */
                 "\033[1;38;5;214m\xe2\x9d\xaf\033[0m "); /* ❯ + space */
    } else if (c->log.fd >= 0) {
        /* cyan bar + ZY pill with recording dot */
        snprintf(buf, cap,
                 "\033[38;5;51m\xe2\x96\x8e"             /* ▎ bar */
                 "\033[48;5;24m\033[1;97m ZY "           /* " ZY " chip */
                 "\033[38;5;198m\xe2\x97\x8f "           /* ● rec dot */
                 "\033[0;49m "                           /* back to normal */
                 "\033[1;38;5;51m\xe2\x9d\xaf\033[0m "); /* ❯ + space */
    } else {
        /* cyan bar + ZY pill, idle */
        snprintf(buf, cap,
                 "\033[38;5;51m\xe2\x96\x8e"             /* ▎ bar */
                 "\033[48;5;24m\033[1;97m ZY "           /* " ZY " chip */
                 "\033[0;49m "                           /* back to normal */
                 "\033[1;38;5;51m\xe2\x9d\xaf\033[0m "); /* ❯ + space */
    }
    return (size_t)visible_len(buf);
}

void draw_input(zt_ctx *c) {
    if (c->tui.rows < 2) return;
    char head[32];
    int  hn = snprintf(head, sizeof head, "\033[%d;1H\033[2K", c->tui.rows);
    if (hn > 0) ob_write(head, (size_t)hn);

    char   pbuf[256];
    size_t pvis = build_prompt(c, pbuf, sizeof pbuf);
    ob_cstr(pbuf);

    size_t cols       = (size_t)c->tui.cols;
    size_t avail      = (pvis + 2 < cols) ? cols - pvis - 1 : 8;

    size_t cursor_abs = c->tui.sent_len + c->tui.cursor;
    size_t start      = 0;
    if (c->tui.input_len > avail) {
        if (cursor_abs > avail / 2) start = cursor_abs - avail / 2;
        if (start + avail > c->tui.input_len) start = c->tui.input_len - avail;
    }

    bool in_sent = false, in_unsent = false;
    for (size_t i = start; i < c->tui.input_len && (i - start) < avail; i++) {
        unsigned char ch = c->tui.input_buf[i];
        unsigned char r  = (ch >= 0x20 && ch < 0x7f) ? ch : '.';
        if (i < c->tui.sent_len) {
            if (!in_sent) {
                ob_cstr("\033[2;38;5;244m");
                in_sent   = true;
                in_unsent = false;
            }
        } else {
            if (!in_unsent) {
                ob_cstr("\033[0;1;97m");
                in_unsent = true;
                in_sent   = false;
            }
        }
        ob_write(&r, 1);
    }
    ob_cstr("\033[0m");

    /* place cursor right after the last displayed char.
     * No \e[?25h here — see draw_hud comment; cursor visibility is
     * managed once at startup/teardown, never per frame. */
    size_t curs_col = pvis + 1;
    if (c->tui.input_len > 0) {
        size_t displayed = c->tui.input_len - start;
        if (displayed > avail) displayed = avail;
        size_t cursor_in_text = cursor_abs - start;
        if (cursor_in_text > displayed) cursor_in_text = displayed;
        curs_col = pvis + cursor_in_text + 1;
    }
    char mv[32];
    int  mvn = snprintf(mv, sizeof mv, "\033[%d;%zuH", c->tui.rows, curs_col);
    if (mvn > 0) ob_write(mv, (size_t)mvn);

    /* Push input state to HTTP peers so the web input bar stays in sync. */
    http_notify_input(c);
}

void apply_layout(zt_ctx *c) {
    char buf[96];
    int  n = snprintf(buf, sizeof buf, "\033[2J\033[2;%dr\033[2;1H\0337", c->tui.rows - 1);
    if (n > 0) ob_write(buf, (size_t)n);
    draw_hud(c);
    draw_input(c);
    ob_flush();
}

/* Centered floating dialog — glassmorphism frosted-glass design.
 * Rounded thin borders, frosted bg, single subtle shadow, warm
 * amber/gold accent palette.  Title row on a slightly lighter strip. */
void draw_dialog(zt_ctx *c, const char *title_icon, const char *title, const char *accent_fg,
                 const char *const *body, int body_n, const char *footer) {
    const char *BG   = "\033[48;2;42;42;42m"; /* frosted glass fill (true gray)  */
    const char *TBG  = "\033[48;2;50;50;50m"; /* title-bar strip (bit lighter)   */
    const char *BORD = "\033[38;2;80;80;80m"; /* subtle dim border               */
    const char *RES  = "\033[0m";
    const char *afg  = accent_fg ? accent_fg : "\033[38;5;178m"; /* gold */

    /* box width: wide enough for longest line + inner padding */
    int need = 0;
    if (title) {
        int tw = visible_len(title);
        if (title_icon) tw += visible_len(title_icon) + 1;
        if (tw > need) need = tw;
    }
    for (int i = 0; i < body_n; i++) {
        int w = visible_len(body[i]);
        if (w > need) need = w;
    }
    if (footer) {
        int w = visible_len(footer);
        if (w > need) need = w;
    }
    int box_w = need + 6;
    if (box_w < 40) box_w = 40;
    if (box_w > c->tui.cols - 2) box_w = c->tui.cols - 2;

    int body_h = body_n + (title ? 2 : 0) + (footer ? 2 : 0);
    int box_h  = body_h + 2; /* +2 borders */
    int top    = (c->tui.rows - box_h) / 2;
    int left   = (c->tui.cols - box_w) / 2;
    if (top < 2) top = 2;
    if (left < 1) left = 1;

    /* ── single-layer soft shadow ── */
    for (int r = 0; r < box_h; r++) {
        char mv[32];
        snprintf(mv, sizeof mv, "\033[%d;%dH", top + 1 + r, left + 1);
        ob_cstr(mv);
        ob_cstr("\033[48;2;28;28;28m"); /* shadow: true neutral dark gray */
        for (int i = 0; i < box_w; i++)
            ob_write(" ", 1);
    }
    ob_cstr(RES);

    char mv[32];

    /* ── top border: rounded thin line ── */
    snprintf(mv, sizeof mv, "\033[%d;%dH", top, left);
    ob_cstr(mv);
    ob_cstr(BG);
    ob_cstr(BORD);
    ob_cstr("\xe2\x95\xad"); /* ╭ */
    for (int i = 0; i < box_w - 2; i++)
        ob_cstr("\xe2\x94\x80"); /* ─ */
    ob_cstr("\xe2\x95\xae");     /* ╮ */
    ob_cstr(RES);

    /* ── body rows ── */
    int row        = top + 1;
    int line_idx   = 0;
    int total_body = body_h;
    for (int br = 0; br < total_body; br++, row++) {
        snprintf(mv, sizeof mv, "\033[%d;%dH", row, left);
        ob_cstr(mv);

        bool        is_title_row = (title && br == 0);
        const char *rbg          = is_title_row ? TBG : BG;

        /* left border */
        ob_cstr(rbg);
        ob_cstr(BORD);
        ob_cstr("\xe2\x94\x82"); /* │ */
        ob_cstr(RES);
        ob_cstr(rbg);
        ob_cstr("  "); /* generous inner padding */

        int cw = 0;
        if (is_title_row) {
            /* ── title: warm gold icon + soft white label ── */
            ob_cstr(afg);
            ob_cstr("\033[1m");
            if (title_icon) {
                ob_cstr(title_icon);
                ob_cstr(" ");
                cw = visible_len(title_icon) + 1;
            }
            ob_cstr("\033[38;5;252m");
            ob_cstr(title);
            cw += visible_len(title);
        } else if (title && br == 1) {
            /* under-title: very faint thin separator */
            ob_cstr("\033[38;5;238m");
            for (int i = 0; i < box_w - 4; i++)
                ob_cstr("\xe2\x94\x80"); /* ─ */
            cw = box_w - 4;
        } else if (footer && br == total_body - 1) {
            /* footer: muted warm */
            ob_cstr("\033[38;5;243m");
            ob_cstr(footer);
            cw = visible_len(footer);
        } else if (footer && br == total_body - 2) {
            /* separator above footer */
            ob_cstr("\033[38;5;238m");
            for (int i = 0; i < box_w - 4; i++)
                ob_cstr("\xe2\x94\x80");
            cw = box_w - 4;
        } else {
            if (line_idx < body_n) {
                const char *content = body[line_idx++];
                ob_cstr(content);
                cw = visible_len(content);
            }
        }

        /* right-pad */
        ob_cstr(RES);
        ob_cstr(rbg);
        int avail = box_w - 4;
        for (int i = cw; i < avail; i++)
            ob_write(" ", 1);

        /* right border */
        ob_cstr(BORD);
        ob_cstr("\xe2\x94\x82"); /* │ */
        ob_cstr(RES);
    }

    /* ── bottom border: rounded thin line ── */
    snprintf(mv, sizeof mv, "\033[%d;%dH", top + box_h - 1, left);
    ob_cstr(mv);
    ob_cstr(BG);
    ob_cstr(BORD);
    ob_cstr("\xe2\x95\xb0"); /* ╰ */
    for (int i = 0; i < box_w - 2; i++)
        ob_cstr("\xe2\x94\x80"); /* ─ */
    ob_cstr("\xe2\x95\xaf");     /* ╯ */
    ob_cstr(RES);

    ob_cstr("\033[?25l");
}

/* Centered popup for Ctrl+A command mode — frosted warm two-column layout. */
void draw_cmd_popup(zt_ctx *c) {
/* Each row: two keybind columns. Format:
 *   <key> bullet <label>   <key> bullet <label>
 * Keys are bold amber, labels soft white, bullet is dim separator. */
#define K(k)                 "\033[1;38;5;214m" k "\033[0;48;2;42;42;42m"
#define D                    "\033[38;5;239m\xe2\x80\xa2\033[0;48;2;42;42;42m"
#define L(s)                 "\033[38;5;250m" s "\033[0;48;2;42;42;42m"
#define ROW2(k1, l1, k2, l2) " " K(k1) " " D " " L(l1) "   " K(k2) " " D " " L(l2)
#define ROW1(k1, l1)         " " K(k1) " " D " " L(l1)
#define SEC(s)                                                                                 \
    "  \033[38;5;239m\xe2\x94\x80\xe2\x94\x80 \033[1;38;5;178m" s                              \
    "\033[0;48;2;42;42;42;38;5;239m "                                                          \
    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2" \
    "\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94" \
    "\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80" \
    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\033[0;48;2;42;"  \
    "42;42m"

    static const char *body[] = {
        "",
        SEC("SESSION"),
        ROW2("x", "exit            ", "p", "pause / resume"),
        ROW2("c", "clear screen    ", "s", "show stats    "),
        ROW2("b", "send break      ", "a", "send Ctrl+A   "),
        "",
        SEC("DISPLAY"),
        ROW2("e", "local echo      ", "h", "hex mode      "),
        ROW2("t", "timestamps      ", "m", "mouse tracking"),
        "",
        SEC("DEVICE"),
        ROW2("l", "log start / stop", "n", "rename log    "),
        ROW2("f", "cycle flow      ", "r", "reconnect     "),
        ROW1("/", "search scrollback"),
        "",
        ROW2("o", "\033[1;38;5;172msettings\033[0;48;2;42;42;42;38;5;250m         ", "k",
             "\033[1;38;5;180mkeybindings\033[0;48;2;42;42;42;38;5;250m     "),
        "",
    };
    draw_dialog(c, NULL,                    /* no icon */
                "zyterm \xc2\xb7 commands", /* title */
                "\033[38;5;178m",           /* gold accent */
                body, (int)(sizeof body / sizeof body[0]), "press key \xc2\xb7 Esc cancel");

#undef K
#undef D
#undef L
#undef ROW2
#undef ROW1
#undef SEC
    c->tui.popup_active = true;
    ob_flush();
}

__attribute__((format(printf, 2, 3))) void set_flash(zt_ctx *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->tui.flash, sizeof c->tui.flash, fmt, ap);
    va_end(ap);
    struct timespec t;
    now(&t);
    c->tui.flash_until = t;
    c->tui.flash_until.tv_sec += 2;
    c->tui.ui_dirty = true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Minicom-style settings menu  (Ctrl+A o)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define SETTINGS_PAGES 4

/* Build-a-row macros (glassmorphism warm palette) */
#define SK(k)   "\033[1;38;5;214m" k "\033[0;48;2;42;42;42m"
#define SD      "\033[38;5;239m\xe2\x80\xa2\033[0;48;2;42;42;42m"
#define SL(s)   "\033[38;5;250m" s "\033[0;48;2;42;42;42m"
#define SV(s)   "\033[1;38;5;178m" s "\033[0;48;2;42;42;42m"
#define SDIM(s) "\033[38;5;243m" s "\033[0;48;2;42;42;42m"

void draw_settings_page(zt_ctx *c) {
    static const char *titles[SETTINGS_PAGES] = {
        "Serial Port",
        "Screen & Display",
        "Keyboard & Misc",
        "Logging",
    };
    static const char *icons[SETTINGS_PAGES] = {
        "\xe2\x97\x88",
        "\xe2\x97\x89",
        "\xe2\x97\x87",
        "\xe2\x97\x8e",
    };
    static const char *accents[SETTINGS_PAGES] = {
        "\033[38;5;214m",
        "\033[38;5;178m",
        "\033[38;5;172m",
        "\033[38;5;180m",
    };

    char        rb[16][256]; /* formatted row buffers */
    const char *body[24];
    int         body_n = 0;
    int         pg     = c->tui.settings_page % SETTINGS_PAGES;

    switch (pg) {
    case 0: { /* ── Serial Port ─────────────────────────────────────── */
        snprintf(rb[0], sizeof rb[0],
                 " " SK("A") " " SD " " SL("Serial Device      : ") SV("%s"),
                 c->serial.device ? c->serial.device : "(none)");
        snprintf(rb[1], sizeof rb[1],
                 " " SK("B") " " SD " " SL("Baud Rate           : ") SV("%u"), c->serial.baud);
        snprintf(rb[2], sizeof rb[2],
                 " " SK("C") " " SD " " SL("Data Bits           : ") SV("%d"),
                 c->serial.data_bits);
        const char *pn = c->serial.parity == 'e'   ? "Even"
                         : c->serial.parity == 'o' ? "Odd"
                                                   : "None";
        snprintf(rb[3], sizeof rb[3],
                 " " SK("D") " " SD " " SL("Parity              : ") SV("%s"), pn);
        snprintf(rb[4], sizeof rb[4],
                 " " SK("E") " " SD " " SL("Stop Bits           : ") SV("%d"),
                 c->serial.stop_bits);
        const char *fn = c->serial.flow == 1   ? "RTS/CTS"
                         : c->serial.flow == 2 ? "XON/XOFF"
                                               : "None";
        snprintf(rb[5], sizeof rb[5],
                 " " SK("F") " " SD " " SL("Flow Control        : ") SV("%s"), fn);
        const char *frm = c->proto.mode == 0   ? "Raw"
                          : c->proto.mode == 1 ? "COBS"
                          : c->proto.mode == 2 ? "SLIP"
                          : c->proto.mode == 3 ? "HDLC"
                                               : "LenPfx";
        snprintf(rb[6], sizeof rb[6],
                 " " SK("G") " " SD " " SL("Frame Mode          : ") SV("%s"), frm);
        const char *cn = c->proto.crc_mode == 0   ? "None"
                         : c->proto.crc_mode == 1 ? "CRC-8"
                         : c->proto.crc_mode == 2 ? "CRC-16"
                                                  : "CRC-32";
        snprintf(rb[7], sizeof rb[7],
                 " " SK("H") " " SD " " SL("CRC Mode            : ") SV("%s"), cn);
        body[body_n++] = "";
        for (int i = 0; i < 8; i++)
            body[body_n++] = rb[i];
        body[body_n++] = "";
        body[body_n++] = SDIM("  Press letter to cycle \xc2\xb7 changes apply live");
        body[body_n++] = "";
        break;
    }
    case 1: { /* ── Screen & Display ────────────────────────────────── */
        snprintf(rb[0], sizeof rb[0],
                 " " SK("A") " " SD " " SL("Color Mode          : ") SV("%s"),
                 c->proto.color_on ? "On" : "Off");
        snprintf(rb[1], sizeof rb[1],
                 " " SK("B") " " SD " " SL("Local Echo          : ") SV("%s"),
                 c->proto.local_echo ? "On" : "Off");
        snprintf(rb[2], sizeof rb[2],
                 " " SK("C") " " SD " " SL("Hex Display         : ") SV("%s"),
                 c->log.hex_mode ? "On" : "Off");
        snprintf(rb[3], sizeof rb[3],
                 " " SK("D") " " SD " " SL("Timestamps          : ") SV("%s"),
                 c->proto.show_ts ? "On" : "Off");
        snprintf(rb[4], sizeof rb[4],
                 " " SK("E") " " SD " " SL("SGR Passthrough     : ") SV("%s"),
                 c->proto.sgr_passthrough ? "On" : "Off");
        snprintf(rb[5], sizeof rb[5],
                 " " SK("F") " " SD " " SL("Raw Passthrough     : ") SV("%s"),
                 c->proto.passthrough ? "On" : "Off");
        snprintf(rb[6], sizeof rb[6],
                 " " SK("G") " " SD " " SL("Mute <dbg>          : ") SV("%s"),
                 c->log.mute_dbg ? "Yes" : "No");
        snprintf(rb[7], sizeof rb[7],
                 " " SK("H") " " SD " " SL("Mute <inf>          : ") SV("%s"),
                 c->log.mute_inf ? "Yes" : "No");
        body[body_n++] = "";
        for (int i = 0; i < 8; i++)
            body[body_n++] = rb[i];
        body[body_n++] = "";
        break;
    }
    case 2: { /* ── Keyboard & Misc ─────────────────────────────────── */
        snprintf(rb[0], sizeof rb[0],
                 " " SK("A") " " SD " " SL("Command Key         : ") SV("Ctrl+A"));
        snprintf(rb[1], sizeof rb[1],
                 " " SK("B") " " SD " " SL("Mouse Capture       : ") SV("%s"),
                 c->tui.mouse_tracking ? "On" : "Off");
        snprintf(rb[2], sizeof rb[2],
                 " " SK("C") " " SD " " SL("Watch Beep          : ") SV("%s"),
                 c->log.watch_beep ? "On" : "Off");
        snprintf(rb[3], sizeof rb[3],
                 " " SK("D") " " SD " " SL("OSC 52 Clipboard    : ") SV("%s"),
                 c->proto.osc52_enabled ? "On" : "Off");
        snprintf(rb[4], sizeof rb[4],
                 " " SK("E") " " SD " " SL("Hyperlinks (OSC 8)  : ") SV("%s"),
                 c->proto.hyperlinks ? "On" : "Off");
        snprintf(rb[5], sizeof rb[5],
                 " " SK("F") " " SD " " SL("Pause               : ") SV("%s"),
                 c->core.paused ? "Yes" : "No");
        snprintf(rb[6], sizeof rb[6],
                 " " SK("G") " " SD " " SL("Auto-reconnect      : ") SV("%s"),
                 c->core.reconnect ? "On" : "Off");
        body[body_n++] = "";
        for (int i = 0; i < 7; i++)
            body[body_n++] = rb[i];
        body[body_n++] = "";
        break;
    }
    case 3: { /* ── Logging ─────────────────────────────────────────── */
        snprintf(rb[0], sizeof rb[0],
                 " " SK("A") " " SD " " SL("Log Status          : ") SV("%s"),
                 c->log.fd >= 0 ? "Active" : "Off");
        snprintf(rb[1], sizeof rb[1],
                 " " SK("B") " " SD " " SL("Log File            : ") SV("%s"),
                 c->log.path ? c->log.path : "(none)");
        const char *lfn = c->log.format == ZT_LOG_JSON  ? "JSON"
                          : c->log.format == ZT_LOG_RAW ? "Raw"
                                                        : "Text";
        snprintf(rb[2], sizeof rb[2],
                 " " SK("C") " " SD " " SL("Log Format          : ") SV("%s"), lfn);
        char rot[32];
        if (c->log.max_bytes > 0)
            snprintf(rot, sizeof rot, "%llu B", (unsigned long long)c->log.max_bytes);
        else
            snprintf(rot, sizeof rot, "Off");
        snprintf(rb[3], sizeof rb[3],
                 " " SK("D") " " SD " " SL("Log Rotation        : ") SV("%s"), rot);
        snprintf(rb[4], sizeof rb[4],
                 " " SK("E") " " SD " " SL("TX Timestamps       : ") SV("%s"),
                 c->proto.tx_ts ? "On" : "Off");
        char wb[32];
        if (c->log.fd >= 0)
            snprintf(wb, sizeof wb, "%llu B", (unsigned long long)c->log.bytes);
        else
            snprintf(wb, sizeof wb, "-");
        snprintf(rb[5], sizeof rb[5], "          " SL("Bytes Written       : ") SV("%s"), wb);
        body[body_n++] = "";
        for (int i = 0; i < 6; i++)
            body[body_n++] = rb[i];
        body[body_n++] = "";
        break;
    }
    }

    /* Footer with page navigation */
    char footer[128];
    snprintf(footer, sizeof footer, "\xe2\x97\x82 / \xe2\x96\xb8 page  [%d/%d]  Esc exit",
             pg + 1, SETTINGS_PAGES);

    char title[80];
    snprintf(title, sizeof title, "zyterm \xc2\xb7 %s", titles[pg]);

    draw_dialog(c, icons[pg], title, accents[pg], body, body_n, footer);
    c->tui.popup_active = true;
    ob_flush();
}

#undef SK
#undef SD
#undef SL
#undef SV
#undef SDIM

/* ═══════════════════════════════════════════════════════════════════════════
 *  Keybindings reference popup  (Ctrl+A k)
 * ═══════════════════════════════════════════════════════════════════════════ */

void draw_keybind_popup(zt_ctx *c) {
#define BK(k)                 "\033[1;38;5;214m" k "\033[0;48;2;42;42;42m"
#define BD                    "\033[38;5;239m\xe2\x80\xa2\033[0;48;2;42;42;42m"
#define BL(s)                 "\033[38;5;250m" s "\033[0;48;2;42;42;42m"
#define BROW2(k1, l1, k2, l2) " " BK(k1) " " BD " " BL(l1) "   " BK(k2) " " BD " " BL(l2)
#define BROW1(k1, l1)         " " BK(k1) " " BD " " BL(l1)
#define BSEC(s)                                                                                \
    "  \033[38;5;239m\xe2\x94\x80\xe2\x94\x80 \033[1;38;5;172m" s                              \
    "\033[0;48;2;42;42;42;38;5;239m "                                                          \
    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2" \
    "\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94" \
    "\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80" \
    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\033[0;48;2;42;"  \
    "42;42m"

    static const char *body[] = {
        "",
        BSEC("CTRL+A COMMANDS"),
        BROW2("x", "exit            ", "p", "pause / resume"),
        BROW2("e", "local echo      ", "h", "hex mode      "),
        BROW2("t", "timestamps      ", "c", "clear screen  "),
        BROW2("b", "send break      ", "s", "show stats    "),
        BROW2("a", "send Ctrl+A     ", "m", "mouse toggle  "),
        BROW2("l", "log toggle      ", "n", "rename log    "),
        BROW2("f", "cycle flow      ", "r", "reconnect     "),
        BROW2("/", "search          ", "Y", "yank / copy   "),
        BROW2("j", "JSON log format ", "F", "frame cycle   "),
        BROW2("K", "CRC cycle       ", "G", "passthrough   "),
        BROW2("D", "mute <dbg>      ", "I", "mute <inf>    "),
        BROW2("+", "add bookmark    ", "[", "bookmark list "),
        BROW2(".", "fuzzy history   ", "Q", "autobaud      "),
        BROW2("o", "settings        ", "k", "this reference"),
        "",
        BSEC("INPUT"),
        BROW2("Enter", "send line  ", "Tab", "complete     "),
        BROW2("Esc", "clear buf    ", "BkSp", "delete char  "),
        BROW2("\xe2\x86\x91/\xe2\x86\x93", "history     ", "\xe2\x86\x90/\xe2\x86\x92",
              "move cursor  "),
        BROW2("PgUp", "scroll up   ", "PgDn", "scroll down  "),
        BROW1("F1-F12", "fire macro"),
        "",
        BSEC("MOUSE"),
        BROW2("drag", "select text ", "wheel", "scroll      "),
        BROW1("r-click", "re-copy selection"),
        "",
    };
    draw_dialog(c, NULL, "zyterm \xc2\xb7 keybindings", "\033[38;5;214m", /* amber accent */
                body, (int)(sizeof body / sizeof body[0]), "Esc to close");

#undef BK
#undef BD
#undef BL
#undef BROW2
#undef BROW1
#undef BSEC
    c->tui.popup_active = true;
    ob_flush();
}
