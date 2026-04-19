/**
 * @file    proto.h
 * @brief   Wire/escape protocols — frame decoders, file-transfer, F-key macros,
 *          OSC 52/8, ANSI SGR pass-through, KGDB raw mode.
 *
 * Module: @c proto/.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#ifndef ZYTERM_INTERNAL_PROTO_H_
#define ZYTERM_INTERNAL_PROTO_H_

#include "log.h"

/* ── proto/framing.c ───────────────────────────────────────────────────── */
void framing_reset(zt_ctx *c);
/** Feed RX bytes into the frame decoder; calls render_rx() per complete frame. */
void framing_feed(zt_ctx *c, const unsigned char *buf, size_t n);
/** Encode + send a TX payload per current frame_mode + crc_mode. */
int         framing_send(zt_ctx *c, const unsigned char *payload, size_t n);
const char *framing_name(zt_frame_mode m);

/* ── proto/macros.c ────────────────────────────────────────────────────── */
int    fkey_index(const unsigned char *buf, size_t n);
size_t expand_escapes(const char *src, char *dst, size_t cap);
void   macro_fire(zt_ctx *c, int fkey_idx);

/* ── proto/xmodem.c ────────────────────────────────────────────────────── */
int xmodem_send(zt_ctx *c, const char *path);
int xmodem_receive(zt_ctx *c, const char *path);
int ymodem_send(zt_ctx *c, const char *path);
int ymodem_receive(zt_ctx *c, const char *dir);
/** ZMODEM via lrzsz (`sz`/`rz`) — shells out and pipes through serial. */
int zmodem_send(zt_ctx *c, const char *path);
int zmodem_receive(zt_ctx *c, const char *dir);

/* ── proto/osc.c ───────────────────────────────────────────────────────── */
void   osc52_copy(zt_ctx *c, const char *buf, size_t n);
size_t osc8_rewrite(const unsigned char *in, size_t n, unsigned char *out, size_t cap);

/* ── proto/clipboard.c ─────────────────────────────────────────────────── */
/** Native X11 selection-owner copy. Returns true if the data was queued
 *  on the CLIPBOARD selection, false on no $DISPLAY / no xcb / OOM. */
bool clipboard_native_set(const char *buf, size_t n);

/* ── proto/sgr_passthrough.c ───────────────────────────────────────────── */
void sgr_filter(zt_ctx *c, const unsigned char *buf, size_t n);

/* ── proto/passthrough.c ───────────────────────────────────────────────── */
void passthrough_enter(zt_ctx *c);
void passthrough_exit(zt_ctx *c);
bool passthrough_handle(zt_ctx *c, const unsigned char *buf, size_t n);

#endif /* ZYTERM_INTERNAL_PROTO_H_ */
