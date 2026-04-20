/**
 * @file    serial.h
 * @brief   Serial port setup, fast I/O paths, kernel UART counters, autobaud.
 *
 * Module: @c serial/. Wraps Linux/macOS-specific syscalls (termios2, BOTHER,
 * IOSSIOSPEED, epoll, splice, TIOCGICOUNT, TIOCMGET) behind portable APIs.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#ifndef ZYTERM_INTERNAL_SERIAL_H_
#define ZYTERM_INTERNAL_SERIAL_H_

#include "core.h"

/* ── serial/serial.c ───────────────────────────────────────────────────── */
int set_custom_baud(int fd, unsigned baud);
int setup_serial(const char *path, unsigned baud, int data_bits, char parity, int stop_bits,
                 int flow);
int apply_flow(int fd, int flow);
int try_reopen_serial(const char *path, unsigned baud, int data_bits, char parity,
                      int stop_bits, int flow);
int reconnect_attempt(zt_ctx *c);

/* ── serial/fastio.c ───────────────────────────────────────────────────── */
int  fastio_init(zt_ctx *c);
void fastio_shutdown(zt_ctx *c);
int  fastio_add_fd(zt_ctx *c, int fd, unsigned events);
int  fastio_del_fd(zt_ctx *c, int fd);
/** Edge-triggered wait. Returns event count or -1. */
int fastio_wait(zt_ctx *c, int timeout_ms, int *out_fds, unsigned *out_events, int max_out);
/** Zero-copy RAW-log fast path. -1 if unsupported on this platform. */
ssize_t fastio_splice_log(zt_ctx *c, int src_fd);

/* ── serial/tty_stats.c ────────────────────────────────────────────────── */
void        tty_stats_poll(zt_ctx *c);  /**< per HUD tick. */
void        tty_stats_flush(zt_ctx *c); /**< TCFLSH on reconnect. */
const char *tty_stats_modem_str(unsigned mask, char *buf, size_t cap);

/* ── serial/autobaud.c ─────────────────────────────────────────────────── */
int autobaud_probe(zt_ctx *c);

/* ── serial/port_discover.c ────────────────────────────────────────────── */
/** Probe a TTY's USB ancestor in sysfs. Returns 1 if both @c vid and @c pid
 *  match (a zero argument means "any"), 0 if no match, -1 if the device has
 *  no USB ancestor (e.g. a real UART or a PTY). */
int port_match_vid_pid(const char *device_path, uint16_t vid, uint16_t pid);

/** Find the first device matching @c glob_pat (e.g. `/dev/ttyUSB*`) and,
 *  if @c vid is non-zero, the given USB vendor:product. Returns a malloc'd
 *  path the caller must free, or NULL when nothing matches. */
char *port_discover(const char *glob_pat, uint16_t vid, uint16_t pid);

/** Re-resolve @c c->serial.device using the current discovery hints
 *  (port_glob / match_vid / match_pid). Returns 1 if the path changed,
 *  0 if unchanged or no hints set, -1 on no-match. */
int port_rediscover(zt_ctx *c);

#endif /* ZYTERM_INTERNAL_SERIAL_H_ */
