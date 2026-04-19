/**
 * @file log_json.c
 * @brief Structured JSONL log output.
 *
 * When `c->log.format == ZT_LOG_JSON`, each RX/TX payload, plus every
 * zyterm notice, is appended to the log file as one JSON object per line:
 *
 *   {"ts":"2025-05-14T09:22:31.443Z","dir":"rx","n":42,"b":"..."}
 *   {"ts":"...","dir":"tx","n":3,"b":"abc"}
 *   {"ts":"...","ev":"reconnect","msg":"opened /dev/ttyUSB0"}
 *
 * Binary bytes are hex-escaped as `\u00xx` only for control bytes;
 * printable ASCII is emitted verbatim. This keeps logs grep-friendly
 * while remaining valid JSON.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static void iso_ts(char out[static 32]) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    snprintf(out, 32, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 1000000L));
}
#pragma GCC diagnostic pop

static void json_escape(const unsigned char *in, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 8 < cap; i++) {
        unsigned char b = in[i];
        if (b == '"' || b == '\\') {
            out[o++] = '\\';
            out[o++] = (char)b;
        } else if (b == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (b == '\r') {
            out[o++] = '\\';
            out[o++] = 'r';
        } else if (b == '\t') {
            out[o++] = '\\';
            out[o++] = 't';
        } else if (b < 0x20 || b == 0x7F)
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", b);
        else
            out[o++] = (char)b;
    }
    out[o] = '\0';
}

static void emit(zt_ctx *c, const char *dir, const unsigned char *buf, size_t n) {
    if (!c || c->log.fd < 0 || c->log.format != ZT_LOG_JSON) return;
    char ts[40];
    iso_ts(ts);
    char esc[8192];
    json_escape(buf, n > 2048 ? 2048 : n, esc, sizeof esc);
    char line[8400];
    int  len =
        snprintf(line, sizeof line, "{\"ts\":\"%s\",\"dir\":\"%s\",\"n\":%zu,\"b\":\"%s\"}\n",
                 ts, dir, n, esc);
    if (len > 0) (void)zt_write_all(c->log.fd, line, (size_t)len);
}

void log_json_rx(zt_ctx *c, const unsigned char *buf, size_t n) {
    emit(c, "rx", buf, n);
}
void log_json_tx(zt_ctx *c, const unsigned char *buf, size_t n) {
    emit(c, "tx", buf, n);
}

void log_json_event(zt_ctx *c, const char *event, const char *fmt, ...) {
    if (!c || c->log.fd < 0 || c->log.format != ZT_LOG_JSON) return;
    char ts[40];
    iso_ts(ts);
    char    msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    char esc[2048];
    json_escape((const unsigned char *)msg, strlen(msg), esc, sizeof esc);
    char line[2200];
    int  len = snprintf(line, sizeof line, "{\"ts\":\"%s\",\"ev\":\"%s\",\"msg\":\"%s\"}\n", ts,
                        event, esc);
    if (len > 0) (void)zt_write_all(c->log.fd, line, (size_t)len);
}
