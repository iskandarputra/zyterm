/**
 * @file    core.h
 * @brief   Cross-cutting helpers — logging, output buffer, signals, terminal,
 *          monotonic time, CRC algorithms.
 *
 * Module: @c core/. Lowest layer of zyterm. Depends only on libc + POSIX.
 * Every other module is allowed to use these symbols.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#ifndef ZYTERM_INTERNAL_CORE_H_
#define ZYTERM_INTERNAL_CORE_H_

#include "zt_ctx.h"

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>

/* ── core/core.c ───────────────────────────────────────────────────────── */

/** printf-style warning to stderr, prefixed `zyterm:`. */
void zt_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** printf-style fatal error to stderr, then `exit(1)`. Never returns. */
void zt_die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

/** Append a one-line trace record to `$ZYTERM_TRACE` if set. */
void zt_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** Loop-safe `write(2)`; retries on short writes. Returns 0 on success. */
int zt_write_all(int fd, const void *buf, size_t n);

/** Convenience for `zt_write_all(fd, s, strlen(s))`. */
int zt_write_cstr(int fd, const char *s);

/** Append bytes to the shared stdout output buffer. */
void ob_write(const void *p, size_t n);

/** Append a C string to the stdout output buffer. */
void ob_cstr(const char *s);

/** Flush the stdout output buffer. */
void ob_flush(void);

/** Register (or clear, with NULL) a tap that observes every byte the
 *  render path is about to write to stdout. Used by the cast recorder
 *  (see @ref cast_record_open). At most one callback is active. */
void ob_set_record_callback(void (*cb)(const unsigned char *buf, size_t n));

/** Difference between two monotonic timespecs, in seconds. */
double ts_diff_sec(const struct timespec *a, const struct timespec *b);

/** Fill @p t with the current monotonic time. */
void now(struct timespec *t);

/* signal & terminal management */
void sig_winch(int s);
void install_signals(void);
void uninstall_signals(void);
void restore_terminal(void);
void setup_stdin_raw(void);

/** UTF-8-aware visible width of @p s with CSI escapes stripped. */
int visible_len(const char *s);

/* ── core/crc.c ────────────────────────────────────────────────────────── */

uint32_t    crc_compute(zt_crc_mode m, const unsigned char *buf, size_t n);
size_t      crc_size(zt_crc_mode m);
const char *crc_name(zt_crc_mode m);

#endif /* ZYTERM_INTERNAL_CORE_H_ */
