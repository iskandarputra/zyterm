/**
 * @file    net.h
 * @brief   Network-facing services — HTTP/SSE/WS bridge, Prometheus metrics
 *          exporter, detach/attach session multiplexer.
 *
 * Module: @c net/.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#ifndef ZYTERM_INTERNAL_NET_H_
#define ZYTERM_INTERNAL_NET_H_

#include "tui.h"

/* ── net/http.c ────────────────────────────────────────────────────────── */
int  http_start(zt_ctx *c, int port);
void http_stop(zt_ctx *c);
void http_tick(zt_ctx *c);
void http_broadcast(zt_ctx *c, const unsigned char *buf, size_t n);
void http_broadcast_tx(zt_ctx *c, const unsigned char *buf, size_t n);
void http_notify_input(zt_ctx *c);

/* ── net/metrics.c ─────────────────────────────────────────────────────── */
int  metrics_start(zt_ctx *c, const char *path);
void metrics_stop(zt_ctx *c);
void metrics_tick(zt_ctx *c);

/* ── net/session.c ─────────────────────────────────────────────────────── */
int  session_detach(zt_ctx *c, const char *name);
int  session_attach(const char *name);
void session_tick(zt_ctx *c);
void session_embed_reset(void);

#endif /* ZYTERM_INTERNAL_NET_H_ */
