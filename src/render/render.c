/**
 * @file render.c
 * @brief RX parsing, Zephyr coloring, line rendering, notices
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zyterm/internal/render.h"

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

/* ------------------------------ RX rendering ----------------------------- */

const char *zephyr_color(const unsigned char *line, size_t len) {
    if (len >= 2 && line[1] == ':') {
        switch (line[0]) {
        case 'E': return "\033[1;31m";
        case 'W': return "\033[1;33m";
        case 'I': return "\033[32m";
        case 'D': return "\033[38;5;244m";
        }
    }
    size_t i = 0;
    if (len > 3 && line[0] == '[') {
        while (i < len && line[i] != ']')
            i++;
        if (i < len) {
            i++;
            if (i < len && line[i] == ' ') i++;
        }
    }
    if (i + 5 <= len && line[i] == '<') {
        const unsigned char *p = line + i + 1;
        if (p[0] == 'e' && p[1] == 'r' && p[2] == 'r' && p[3] == '>') return "\033[1;31m";
        if (p[0] == 'w' && p[1] == 'r' && p[2] == 'n' && p[3] == '>') return "\033[1;33m";
        if (p[0] == 'i' && p[1] == 'n' && p[2] == 'f' && p[3] == '>') return "\033[32m";
        if (p[0] == 'd' && p[1] == 'b' && p[2] == 'g' && p[3] == '>') return "\033[38;5;244m";
    }
    return "";
}

void emit_ts(zt_ctx *c) {
    if (!c->proto.show_ts) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    const char *hms = zt_cached_hhmmss(ts.tv_sec);
    char        s[40];
    int         sn = snprintf(s, sizeof s, "\033[38;5;240m[%s.%03ld]\033[0m ", hms,
                              (long)(ts.tv_nsec / 1000000));
    if (sn > 0) ob_write(s, (size_t)sn);
}

/** Emit a scrollback/log line with optional timestamp dim-coloring and
 *  Zephyr log-level color. Detects a raw [HH:MM:SS.mmm] prefix (15 chars)
 *  at the start of the buffer and renders it in dim gray; the remaining
 *  content is wrapped in the auto-detected Zephyr color if color_on. */
void emit_colored_line(zt_ctx *c, const unsigned char *line, size_t len) {
    size_t off = 0;
    /* Detect embedded raw timestamp prefix: [XX:XX:XX.XXX]  (15 chars).
     * Always stored in scrollback; only rendered when show_ts is on. */
    if (len >= 15 && line[0] == '[' && line[3] == ':' && line[6] == ':' && line[9] == '.' &&
        line[13] == ']' && line[14] == ' ') {
        if (c->proto.show_ts) {
            ob_cstr("\033[38;5;240m");
            ob_write(line, 15);
            ob_cstr("\033[0m");
        }
        off = 15; /* always skip past the prefix in the buffer */
    }
    const unsigned char *content = line + off;
    size_t               clen    = len - off;
    const char          *color   = c->proto.color_on ? zephyr_color(content, clen) : "";
    if (*color) ob_cstr(color);
    ob_write(content, clen);
    if (*color) ob_cstr("\033[0m");
}

void flush_line(zt_ctx *c) {
    /* ── timestamp embedding ──────────────────────────────────────────── *
     * ALWAYS prepend [HH:MM:SS.mmm] to log_line before scrollback_push()
     * so every line in scrollback carries its arrival time. The render
     * paths (emit_colored_line) conditionally show or hide the prefix
     * based on c->proto.show_ts, letting the user toggle retroactively.        */
    if (c->log.line_len > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        const char *hms = zt_cached_hhmmss(ts.tv_sec);
        char        prefix[20];
        int         pn =
            snprintf(prefix, sizeof prefix, "[%s.%03ld] ", hms, (long)(ts.tv_nsec / 1000000));
        if (pn > 0 && (size_t)pn + c->log.line_len <= ZT_LINEBUF_CAP) {
            memmove(c->log.line + pn, c->log.line, c->log.line_len);
            memcpy(c->log.line, prefix, (size_t)pn);
            c->log.line_len += (size_t)pn;
        }
    }

    /* ADR-0009: if a device SGR was emitted on this line, close it with a
     * reset so device colour can't bleed into the HUD or the next line.
     * Stored in the line so live render and scrollback replay both contain it. */
    if (c->proto.sgr_in_line) {
        if (c->log.line_len + 4 <= ZT_LINEBUF_CAP) {
            memcpy(c->log.line + c->log.line_len, "\033[0m", 4);
            c->log.line_len += 4;
        }
        c->proto.sgr_in_line = false;
    }

    scrollback_push(c);

    int watch_idx = watch_match(c, c->log.line, c->log.line_len);
    /* Fire per-line event hooks through the ctx sink (wired to hooks_on_line by
     * the loop) so the render layer doesn't name the ext-layer symbol. */
    if (c->core.line_hook) c->core.line_hook(c, c->log.line, c->log.line_len);

    if (c->tui.sb_offset == 0 && !c->tui.popup_active) {
        if (watch_idx) {
            /* watch hit: reverse video + color per watch slot */
            static const char *wc[ZT_WATCH_MAX] = {
                "\033[30;103m", /* yellow bg */
                "\033[30;101m", /* red bg */
                "\033[30;102m", /* green bg */
                "\033[30;106m", /* cyan bg */
                "\033[30;105m", /* magenta bg */
                "\033[30;104m", /* blue bg */
                "\033[30;47m",  /* white bg */
                "\033[30;100m", /* dim bg */
            };
            ob_cstr(wc[(watch_idx - 1) % ZT_WATCH_MAX]);
            ob_write(c->log.line, c->log.line_len);
            ob_cstr("\033[0m");
            if (c->log.watch_beep) ob_cstr("\007");
        } else {
            emit_colored_line(c, c->log.line, c->log.line_len);
        }
        ob_cstr("\r\n");
    } else if (watch_idx && c->log.watch_beep) {
        ob_cstr("\007");
    }
    c->core.rx_lines++;
    c->log.line_len = 0;
}

/* ----------------------------- hex dump ---------------------------------- */

/** Return an SGR color code for a raw byte value in the hex dump.
 *  - 0x00       → dim gray   (null bytes stand out less)
 *  - 0x20-0x7E  → green      (printable ASCII)
 *  - 0x01-0x1F, 0x7F → yellow (control chars)
 *  - 0x80-0xFF  → magenta    (high / extended bytes)                      */
static const char *hex_byte_color(unsigned char b) {
    if (b == 0x00) return "\033[38;5;240m";
    if (b >= 0x20 && b <= 0x7E) return "\033[32m";
    if (b <= 0x1F || b == 0x7F) return "\033[33m";
    return "\033[35m"; /* 0x80-0xFF */
}

/** Format the accumulated hex_row into a colorized hexdump line and push it
 *  through flush_line() so it enters scrollback (selection, scroll, log).
 *
 *  Visual format (colors embedded via SGR):
 *    XXXXXXXX  XX XX XX XX XX XX XX XX  XX XX XX XX XX XX XX XX  |................|
 *    ^^^^^^^^  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^  ^^^^^^^^^^^^^^^^^
 *    cyan      per-byte color                                     dim dots / text  */
void hex_flush_row(zt_ctx *c) {
    if (c->log.hex_row_len == 0) return;
    char tmp[1024];
    int  pos = 0;

#define HEX_APP(fmt, ...)                                                                      \
    pos += snprintf(tmp + pos, sizeof tmp - (size_t)pos, fmt, ##__VA_ARGS__)

    /* Offset address — cyan */
    HEX_APP("\033[36m%08llX\033[0m  ", (unsigned long long)c->log.hex_offset);

    /* First 8 hex bytes */
    for (unsigned j = 0; j < 8; j++) {
        if (j < c->log.hex_row_len)
            HEX_APP("%s%02X\033[0m ", hex_byte_color(c->log.hex_row[j]), c->log.hex_row[j]);
        else
            HEX_APP("   ");
    }
    tmp[pos++] = ' '; /* group gap */

    /* Next 8 hex bytes */
    for (unsigned j = 8; j < 16; j++) {
        if (j < c->log.hex_row_len)
            HEX_APP("%s%02X\033[0m ", hex_byte_color(c->log.hex_row[j]), c->log.hex_row[j]);
        else
            HEX_APP("   ");
    }
    tmp[pos++] = ' '; /* pad before ASCII */

    /* ASCII sidebar — dim separators, per-byte coloring */
    HEX_APP("\033[38;5;240m|\033[0m");
    for (unsigned j = 0; j < c->log.hex_row_len; j++) {
        unsigned char b = c->log.hex_row[j];
        if (b >= 0x20 && b <= 0x7E)
            HEX_APP("\033[37m%c\033[0m", (char)b);
        else
            HEX_APP("\033[38;5;240m.\033[0m");
    }
    HEX_APP("\033[38;5;240m|\033[0m");
#undef HEX_APP

    /* Copy into log_line and flush through the normal path
     * (scrollback_push → watch_match → screen write). */
    size_t len = (size_t)pos;
    if (len > ZT_LINEBUF_CAP) len = ZT_LINEBUF_CAP;
    memcpy(c->log.line, tmp, len);
    c->log.line_len = len;
    flush_line(c);

    c->log.hex_offset += c->log.hex_row_len;
    c->log.hex_row_len = 0;
}

/* Append one byte to the assembling RX line, flushing to scrollback when the
 * fixed line buffer fills (every write stays bounded — INVARIANTS §5). */
static void rx_line_putc(zt_ctx *c, unsigned char b) {
    if (c->log.line_len >= ZT_LINEBUF_CAP) flush_line(c);
    c->log.line[c->log.line_len++] = b;
}

/* Render one byte that is NOT part of an allowed escape:
 *   - ESC/DEL and other C0 controls  → inert cat -v caret notation (^[, ^G, ^?);
 *   - \t and printable ASCII          → pass through;
 *   - valid UTF-8 (lead + expected continuations, tracked in proto.utf8_cont)
 *                                      → pass through;
 *   - a standalone C1 control (0x80-0x9F) or other stray high byte → inert
 *     cat -v M- notation, so an 8-bit OSC/CSI/DCS introducer (0x9D/0x9B/0x90…)
 *     can't drive a C1-honoring terminal past the escape filter (ZT-041).
 * Shared by STRICT mode and the SGR-filter's neutralized paths.
 * (ZT-003 / ADR-0009 / INVARIANTS §6.) */
static void emit_inert_byte(zt_ctx *c, unsigned char b) {
    if (b == 0x1B || b == 0x7F || (b < 0x20 && b != '\t')) {
        rx_line_putc(c, '^');
        rx_line_putc(c, (unsigned char)(b == 0x7F ? '?' : b + 0x40));
        c->proto.utf8_cont = 0;
        return;
    }
    if (b < 0x80) { /* printable ASCII or \t */
        rx_line_putc(c, b);
        c->proto.utf8_cont = 0;
        return;
    }
    /* b >= 0x80. A continuation byte (10xxxxxx) is only legitimate when a lead
     * byte is still expecting it; otherwise it's stray. */
    if (c->proto.utf8_cont > 0 && (b & 0xC0) == 0x80) {
        rx_line_putc(c, b);
        c->proto.utf8_cont--;
        return;
    }
    if ((b & 0xE0) == 0xC0) { /* 2-byte lead */
        c->proto.utf8_cont = 1;
        rx_line_putc(c, b);
        return;
    }
    if ((b & 0xF0) == 0xE0) { /* 3-byte lead */
        c->proto.utf8_cont = 2;
        rx_line_putc(c, b);
        return;
    }
    if ((b & 0xF8) == 0xF0) { /* 4-byte lead */
        c->proto.utf8_cont = 3;
        rx_line_putc(c, b);
        return;
    }
    /* Stray high byte: a standalone C1 control, an unexpected continuation, or
     * an invalid lead (0xC0/0xC1/0xF8-0xFF). Neutralize as cat -v M- notation. */
    c->proto.utf8_cont = 0;
    rx_line_putc(c, 'M');
    rx_line_putc(c, '-');
    if (b <= 0x9F) { /* C1 control → M-^X */
        rx_line_putc(c, '^');
        rx_line_putc(c, (unsigned char)((b - 0x80) + 0x40));
    } else { /* 0xA0-0xFF → M-<printable> */
        rx_line_putc(c, (unsigned char)(b - 0x80));
    }
}

/* Store a validated device SGR sequence verbatim in the line buffer so it
 * renders in-position when the line flushes (and colours scrollback too).
 * Flushes first if it wouldn't fit, so a sequence is never split. Safe: only
 * whitelisted SGR (ADR-0009) ever reaches here, so scrollback replay cannot
 * re-inject a dangerous escape. */
static void put_sgr(zt_ctx *c, const unsigned char *seq, size_t len) {
    if (c->log.line_len + len > ZT_LINEBUF_CAP) flush_line(c);
    for (size_t k = 0; k < len && c->log.line_len < ZT_LINEBUF_CAP; k++)
        c->log.line[c->log.line_len++] = seq[k];
    c->proto.sgr_in_line = true;
}

void render_rx(zt_ctx *c, const unsigned char *buf, size_t n) {
    c->core.rx_bytes += n;
    if (c->core.paused) return;

    /* when popup is on-screen: still count bytes + push to scrollback
     * (via flush_line), but suppress screen writes so popup stays on top */
    bool live = (c->tui.sb_offset == 0 && !c->tui.popup_active);
    if (live) ob_cstr("\0338");

    if (c->log.hex_mode) {
        /* Accumulate bytes into hex_row[16]. When full, format a
         * proper hex dump line into log_line and call flush_line() so
         * it enters scrollback (enabling selection, watch, log, etc.).
         *
         * Format (78 chars, fits 80-col terminal):
         * XXXXXXXX  XX XX XX XX XX XX XX XX  XX XX XX XX XX XX XX XX  |................|
         */
        for (size_t i = 0; i < n; i++) {
            c->log.hex_row[c->log.hex_row_len++] = buf[i];
            if (c->log.hex_row_len == 16) {
                hex_flush_row(c);
            }
        }
    } else {
        /* ZT-003 / ADR-0009 (INVARIANTS §6): device RX is untrusted. Three
         * modes decide how device escapes reach the operator's terminal:
         *   ESC_RAW    (passthrough)     — everything verbatim (KGDB/full TUI);
         *   ESC_SGR    (sgr_passthrough) — only well-formed SGR passes, every
         *                                  other escape neutralized (DEFAULT);
         *   ESC_STRICT (neither)         — neutralize every escape.
         * passthrough wins over sgr_passthrough. */
        enum {
            ESC_STRICT,
            ESC_RAW,
            ESC_SGR
        } esc_mode = c->proto.passthrough       ? ESC_RAW
                     : c->proto.sgr_passthrough ? ESC_SGR
                                                : ESC_STRICT;
        for (size_t i = 0; i < n; i++) {
            unsigned char b = buf[i];

            /* Tab completion echo capture: after Tab, Zephyr echoes the
             * flushed chars (skip those) then the completion suffix
             * (append to buffer).  Stop at [, \r, \n, ESC — all mark
             * the start of log output, not completion text. */
            if (c->proto.tab_echo) {
                if (b == '[' || b == '\r' || b == '\n' || b == 0x1B) {
                    c->proto.tab_echo = false;
                } else if (b >= 0x20 && b < 0x7F) {
                    if (c->proto.tab_skip > 0) {
                        c->proto.tab_skip--;
                    } else if (c->tui.input_len < ZT_INPUT_CAP - 1) {
                        c->tui.input_buf[c->tui.input_len++] = b;
                        c->tui.sent_len++;
                    }
                }
                /* \b from device — undo one captured char */
                if ((b == '\b' || b == 0x7F) && c->proto.tab_skip == 0) {
                    if (c->tui.input_len > 0 && c->tui.sent_len > 0) {
                        c->tui.input_len--;
                        c->tui.sent_len--;
                    }
                }
            }

            /* Any ESC starts a fresh escape sequence: drop a pending UTF-8
             * continuation expectation so a multibyte char truncated by an ESC
             * can't make emit_inert_byte() later mistake a C1 introducer for a
             * continuation byte (ZT-041). \r/\n do the same below. */
            if (b == 0x1B) c->proto.utf8_cont = 0;

            /* RAW: trusted device — \r dropped, \n flushes, all else verbatim. */
            if (esc_mode == ESC_RAW) {
                if (b == '\r') continue;
                if (b == '\n') {
                    flush_line(c);
                    continue;
                }
                rx_line_putc(c, b);
                continue;
            }

            /* SGR-filter: route ESC and mid-sequence bytes through the bounded
             * parser FIRST (so a \r/\n mid-CSI aborts the sequence rather than
             * stranding parser state into the next line). Only well-formed SGR
             * survives; REPROCESS loops back after neutralizing the rest. */
        sgr_reprocess:
            if (esc_mode == ESC_SGR && (c->proto.sgr.state != ZT_SGR_NONE || b == 0x1B)) {
                unsigned char seq[ZT_SGR_PARAM_CAP + 4];
                size_t        sl = 0;
                switch (sgr_feed(&c->proto.sgr, b, seq, &sl)) {
                case ZT_SGR_ACT_HOLD: continue;
                case ZT_SGR_ACT_EMIT_SGR: put_sgr(c, seq, sl); continue;
                case ZT_SGR_ACT_INERT:
                    for (size_t k = 0; k < sl; k++)
                        emit_inert_byte(c, seq[k]);
                    continue;
                case ZT_SGR_ACT_REPROCESS:
                    for (size_t k = 0; k < sl; k++)
                        emit_inert_byte(c, seq[k]);
                    goto sgr_reprocess; /* parser reset to NONE; re-handle b */
                }
            }

            /* STRICT default-deny, and SGR mode's non-sequence bytes: \r/\n
             * handled here; ESC/C0/DEL → inert caret notation; \t/printable pass. */
            if (b == '\r') {
                c->proto.utf8_cont = 0;
                continue;
            }
            if (b == '\n') {
                c->proto.utf8_cont = 0;
                flush_line(c);
                continue;
            }
            emit_inert_byte(c, b);
        }
    }
    if (live) ob_cstr("\0337");
    c->tui.ui_dirty = true;
}

/* rx_ingest() — the per-chunk RX dispatcher — moved to loop/runtime.c
 * (INVARIANTS §8): it orchestrates the ext (filter, hooks) and net
 * (http_broadcast) layers above render, so it belongs in the loop layer that
 * already owns the read loop; it's only ever called from runtime.c. */

/* log_notice() moved to core.c (INVARIANTS §8 — it uses only the core output
 * buffer + a ctx flag, so every layer can call down to it). Declared in core.h. */
