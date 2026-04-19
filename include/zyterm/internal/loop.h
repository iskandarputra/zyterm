/**
 * @file    loop.h
 * @brief   Event loop primitives — keyboard input parsing, send pipeline,
 *          optional RX reader thread, top-level run modes.
 *
 * Module: @c loop/. Topmost internal module; @c main.c is the only thing
 * allowed above it.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#ifndef ZYTERM_INTERNAL_LOOP_H_
#define ZYTERM_INTERNAL_LOOP_H_

#include "ext.h"

/* ── loop/input.c ──────────────────────────────────────────────────────── */
void load_history_into_buf(zt_ctx *c);
void handle_cmd_key(zt_ctx *c, unsigned char k);
void handle_settings_key(zt_ctx *c, const unsigned char *buf, size_t n);
void handle_escape_seq(zt_ctx *c, const unsigned char *buf, size_t n);
void insert_char(zt_ctx *c, unsigned char k);
void delete_before_cursor(zt_ctx *c);
void handle_stdin_chunk(zt_ctx *c, const unsigned char *buf, size_t n);

/* ── loop/send.c ───────────────────────────────────────────────────────── */
void trickle_send(zt_ctx *c, const unsigned char *buf, size_t n);
void direct_send(zt_ctx *c, const unsigned char *buf, size_t n);
void flush_unsent(zt_ctx *c);

/* ── loop/rx_thread.c ──────────────────────────────────────────────────── */
int    rx_thread_start(zt_ctx *c);
void   rx_thread_stop(zt_ctx *c);
size_t rx_thread_drain(zt_ctx *c, unsigned char *dst, size_t cap);

/* ── loop/runtime.c ────────────────────────────────────────────────────── */
int run_interactive(zt_ctx *c);
int run_dump(zt_ctx *c, int seconds);
int run_replay(zt_ctx *c);

#endif /* ZYTERM_INTERNAL_LOOP_H_ */
