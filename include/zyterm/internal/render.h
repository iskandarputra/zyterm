/**
 * @file    render.h
 * @brief   RX byte-stream to screen rendering, throughput sparkline.
 *
 * Module: @c render/.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#ifndef ZYTERM_INTERNAL_RENDER_H_
#define ZYTERM_INTERNAL_RENDER_H_

#include "proto.h"

/* ── render/render.c ───────────────────────────────────────────────────── */
const char *zephyr_color(const unsigned char *line, size_t len);
void        emit_ts(zt_ctx *c);
void        flush_line(zt_ctx *c);
void        hex_flush_row(zt_ctx *c);
void        emit_colored_line(zt_ctx *c, const unsigned char *line, size_t len);
void        render_rx(zt_ctx *c, const unsigned char *buf, size_t n);
void        rx_ingest(zt_ctx *c, const unsigned char *buf, size_t n);

/* ── render/sparkline.c ────────────────────────────────────────────────── */
void        sparkline_push(zt_ctx *c, uint64_t bps);
const char *sparkline_render(const zt_ctx *c, char *out, size_t cap);

#endif /* ZYTERM_INTERNAL_RENDER_H_ */
