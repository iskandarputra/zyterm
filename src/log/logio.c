/**
 * @file logio.c
 * @brief Log file, watch patterns, command history
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

/* ------------------------------ log file --------------------------------- */

void log_rotate_if_needed(zt_ctx *c) {
    if (c->log.fd < 0 || !c->log.max_bytes || c->log.bytes < c->log.max_bytes) return;
    if (!c->log.path) return;
    char rot[PATH_MAX_LEN];
    snprintf(rot, sizeof rot, "%s.1", c->log.path);
    close(c->log.fd);
    c->log.fd = -1;
    (void)rename(c->log.path, rot);
    c->log.fd         = open(c->log.path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    c->log.bytes      = 0;
    c->log.line_start = true;
}

void log_write_raw(zt_ctx *c, const unsigned char *buf, size_t n) {
    if (c->log.fd < 0 || !n) return;
    ssize_t w = write(c->log.fd, buf, n);
    if (w > 0) c->log.bytes += (uint64_t)w;
}

void log_emit_ts(zt_ctx *c, const char *tag) {
    if (c->log.fd < 0) return;
    struct timespec ts;
    struct tm       tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    char s[64];
    int  sn = snprintf(s, sizeof s, "[%04d-%02d-%02d %02d:%02d:%02d.%03ld]%s ",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                       tm.tm_sec, (long)(ts.tv_nsec / 1000000), tag ? tag : "");
    if (sn > 0) log_write_raw(c, (unsigned char *)s, (size_t)sn);
}

/* RX path — adds timestamp at line starts */
void log_write(zt_ctx *c, const unsigned char *buf, size_t n) {
    if (c->log.fd < 0 || !n) return;
    for (size_t i = 0; i < n; i++) {
        if (c->log.line_start) {
            log_emit_ts(c, NULL);
            c->log.line_start = false;
        }
        log_write_raw(c, &buf[i], 1);
        if (buf[i] == '\n') c->log.line_start = true;
    }
    log_rotate_if_needed(c);
}

/* TX path — logs what we send, prefixed with -> when tx_ts is on */
void log_write_tx(zt_ctx *c, const unsigned char *buf, size_t n) {
    if (c->log.fd < 0 || !n || !c->proto.tx_ts) return;
    for (size_t i = 0; i < n; i++) {
        if (c->proto.tx_line_start) {
            log_emit_ts(c, " ->");
            c->proto.tx_line_start = false;
        }
        log_write_raw(c, &buf[i], 1);
        if (buf[i] == '\n' || buf[i] == '\r') c->proto.tx_line_start = true;
    }
    log_rotate_if_needed(c);
}

/* ------------------------------ pattern match ---------------------------- */

int watch_match(const zt_ctx *c, const unsigned char *line, size_t len) {
    for (int i = 0; i < c->log.watch_count; i++) {
        const char *p = c->log.watch[i];
        if (!p || !*p) continue;
        size_t plen = strlen(p);
        if (plen > len) continue;
        for (size_t k = 0; k + plen <= len; k++)
            if (memcmp(line + k, p, plen) == 0) return i + 1;
    }
    return 0;
}

/* ------------------------------ history ---------------------------------- */

void history_push(zt_ctx *c, const unsigned char *buf, size_t n) {
    if (!n) return;
    if (c->tui.hist_count > 0) {
        int last = (c->tui.hist_head + c->tui.hist_count - 1) % ZT_HISTORY_CAP;
        if (c->tui.hist[last] && strlen(c->tui.hist[last]) == n &&
            memcmp(c->tui.hist[last], buf, n) == 0)
            return;
    }
    char *s = malloc(n + 1);
    if (!s) return;
    memcpy(s, buf, n);
    s[n] = 0;
    if (c->tui.hist_count < ZT_HISTORY_CAP) {
        int idx          = (c->tui.hist_head + c->tui.hist_count) % ZT_HISTORY_CAP;
        c->tui.hist[idx] = s;
        c->tui.hist_count++;
    } else {
        free(c->tui.hist[c->tui.hist_head]);
        c->tui.hist[c->tui.hist_head] = s;
        c->tui.hist_head              = (c->tui.hist_head + 1) % ZT_HISTORY_CAP;
    }
}

const char *history_at(zt_ctx *c, int back) {
    if (back <= 0 || back > c->tui.hist_count) return NULL;
    int idx = (c->tui.hist_head + c->tui.hist_count - back) % ZT_HISTORY_CAP;
    return c->tui.hist[idx];
}

void history_free(zt_ctx *c) {
    for (int i = 0; i < ZT_HISTORY_CAP; i++) {
        free(c->tui.hist[i]);
        c->tui.hist[i] = NULL;
    }
}
