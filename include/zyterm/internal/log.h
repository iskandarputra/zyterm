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
void        log_rotate_if_needed(zt_ctx *c);
void        log_write_raw(zt_ctx *c, const unsigned char *buf, size_t n);
void        log_emit_ts(zt_ctx *c, const char *tag);
void        log_write(zt_ctx *c, const unsigned char *buf, size_t n);
void        log_write_tx(zt_ctx *c, const unsigned char *buf, size_t n);
void        log_notice(zt_ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int         watch_match(const zt_ctx *c, const unsigned char *line, size_t len);
void        history_push(zt_ctx *c, const unsigned char *buf, size_t n);
const char *history_at(zt_ctx *c, int back);
void        history_free(zt_ctx *c);

/* ── log/log_json.c ────────────────────────────────────────────────────── */
void log_json_rx(zt_ctx *c, const unsigned char *buf, size_t n);
void log_json_tx(zt_ctx *c, const unsigned char *buf, size_t n);
void log_json_event(zt_ctx *c, const char *event, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* ── log/scrollback.c ──────────────────────────────────────────────────── */
void scrollback_push(zt_ctx *c);
void scrollback_free(zt_ctx *c);
void redraw_scrollback(zt_ctx *c);
void scroll_up(zt_ctx *c, int lines);
void scroll_down(zt_ctx *c, int lines);
void leave_scroll(zt_ctx *c);

/* In-app text selection (mouse-driven). All coordinates are 1-based screen
 * cells; row must be inside the body region (2..rows-1). */
void selection_begin(zt_ctx *c, int row, int col);
void selection_extend(zt_ctx *c, int row, int col);
void selection_finish(zt_ctx *c); /**< Release -> build text + OSC 52 copy. */
void selection_clear(zt_ctx *c);
void selection_copy(zt_ctx *c); /**< Re-copy current selection (right-click). */

#endif /* ZYTERM_INTERNAL_LOG_H_ */
