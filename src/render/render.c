/**
 * @file render.c
 * @brief RX parsing, Zephyr coloring, line rendering, notices
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
    struct tm       tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    char s[40];
    int  sn = snprintf(s, sizeof s, "\033[38;5;240m[%02d:%02d:%02d.%03ld]\033[0m ", tm.tm_hour,
                       tm.tm_min, tm.tm_sec, (long)(ts.tv_nsec / 1000000));
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
        struct tm       tm;
        clock_gettime(CLOCK_REALTIME, &ts);
        localtime_r(&ts.tv_sec, &tm);
        char prefix[20];
        int  pn = snprintf(prefix, sizeof prefix, "[%02d:%02d:%02d.%03ld] ", tm.tm_hour,
                           tm.tm_min, tm.tm_sec, (long)(ts.tv_nsec / 1000000));
        if (pn > 0 && (size_t)pn + c->log.line_len <= ZT_LINEBUF_CAP) {
            memmove(c->log.line + pn, c->log.line, c->log.line_len);
            memcpy(c->log.line, prefix, (size_t)pn);
            c->log.line_len += (size_t)pn;
        }
    }

    scrollback_push(c);

    int watch_idx = watch_match(c, c->log.line, c->log.line_len);

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

            if (b == '\r') continue;
            if (b == '\n') {
                flush_line(c);
                continue;
            }
            if (c->log.line_len < ZT_LINEBUF_CAP)
                c->log.line[c->log.line_len++] = b;
            else {
                flush_line(c);
                c->log.line[c->log.line_len++] = b;
            }
        }
    }
    if (live) ob_cstr("\0337");
    c->tui.ui_dirty = true;
}

/* Single RX ingestion dispatcher. Used by runtime.c for each chunk read
 * from the serial device. Responsibilities:
 *   1. HTTP / JSONL side-channels get the raw bytes first.
 *   2. If a filter subprocess is running, bytes go through it and the
 *      transformed output re-enters via filter_drain() → render_rx().
 *   3. If a frame_mode is configured (COBS/SLIP/HDLC/lenpfx), bytes go
 *      through framing_feed() which calls render_rx() only with
 *      complete CRC-verified payloads.
 *   4. Otherwise, straight to render_rx().
 * Recursion is impossible because framing.c / filter.c always deliver
 * raw line content via render_rx(), never via rx_ingest(). */
void rx_ingest(zt_ctx *c, const unsigned char *buf, size_t n) {
    if (!c || !buf || n == 0) return;
    if (c->log.format == ZT_LOG_JSON) log_json_rx(c, buf, n);
    if (c->net.http_fd >= 0) http_broadcast(c, buf, n);
    if (c->ext.filter_pid > 0) {
        filter_feed(c, buf, n);
        return;
    }
    if (c->proto.mode != ZT_FRAME_RAW) {
        framing_feed(c, buf, n);
        return;
    }
    render_rx(c, buf, n);
}

__attribute__((format(printf, 2, 3))) void log_notice(zt_ctx *c, const char *fmt, ...) {
    char    b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    ob_cstr("\0338\033[38;5;60m\xe2\x94\x82\033[0m \033[38;5;245m");
    ob_write(b, (size_t)n);
    ob_cstr("\033[0m\r\n\0337");
    c->tui.ui_dirty = true;
}
