/**
 * @file    log.h
 * @brief   Persistent log file, JSONL emit, scrollback ring buffer.
 *
 * Module: @c log/.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#ifndef ZYTERM_INTERNAL_LOG_H_
#define ZYTERM_INTERNAL_LOG_H_

#include "serial.h"

/* ── log/logio.c ───────────────────────────────────────────────────────── */
void log_rotate_if_needed(zt_ctx *c);
void log_write_raw(zt_ctx *c, const unsigned char *buf, size_t n);
void log_emit_ts(zt_ctx *c, const char *tag);
void log_write(zt_ctx *c, const unsigned char *buf, size_t n);
void log_write_tx(zt_ctx *c, const unsigned char *buf, size_t n);
/* log_notice() moved to core.h (cross-cutting notification primitive). */
int         watch_match(const zt_ctx *c, const unsigned char *line, size_t len);
void        history_push(zt_ctx *c, const unsigned char *buf, size_t n);
const char *history_at(zt_ctx *c, int back);
void        history_free(zt_ctx *c);

/* ── log/log_json.c ────────────────────────────────────────────────────── */
void log_json_rx(zt_ctx *c, const unsigned char *buf, size_t n);
void log_json_tx(zt_ctx *c, const unsigned char *buf, size_t n);
void log_json_event(zt_ctx *c, const char *event, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* ── log/record_cast.c ─────────────────────────────────────────────────── */
/** Open an asciinema cast v2 recording at @p path. Writes the JSON header
 *  immediately. Returns 0 on success, -1 on error (errno set). */
int cast_record_open(zt_ctx *c, const char *path);
/** Append rendered output bytes (typically called from ob_flush). Safe to
 *  call when no recording is active — becomes a no-op. */
void cast_record_o(const unsigned char *buf, size_t n);
/** Flush buffered events and close the file. Safe to call multiple times. */
void cast_record_close(zt_ctx *c);

/* ── log/scrollback.c ──────────────────────────────────────────────────── */
/* Storage only — the line ring. The viewport/selection API is in tui.h
 * (tui/scrollback_view.c). */
void scrollback_push(zt_ctx *c);
void scrollback_free(zt_ctx *c);

#endif /* ZYTERM_INTERNAL_LOG_H_ */
