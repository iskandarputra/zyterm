/**
 * @file sparkline.c
 * @brief Unicode sparkline for HUD throughput visualization.
 *
 * Uses U+2581..U+2588 (eight block shades) mapped against the live
 * max in the rolling history window. Called every HUD refresh with
 * the current RX bytes/sec.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"
#include <stdio.h>
#include <string.h>

/* U+2581 LOWER ONE EIGHTH BLOCK through U+2588 FULL BLOCK */
static const char *kBlocks[8] = {"\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83",
                                 "\xe2\x96\x84", "\xe2\x96\x85", "\xe2\x96\x86",
                                 "\xe2\x96\x87", "\xe2\x96\x88"};

void               sparkline_push(zt_ctx *c, uint64_t bps) {
    if (!c) return;
    c->tui.rx_bps_hist[c->tui.rx_bps_head] = bps;
    c->tui.rx_bps_head = (c->tui.rx_bps_head + 1) % ZT_SPARK_HIST;
}

const char *sparkline_render(const zt_ctx *c, char *out, size_t cap) {
    if (!c || !out || cap < 1) return "";
    uint64_t mx = 1;
    for (int i = 0; i < ZT_SPARK_HIST; i++)
        if (c->tui.rx_bps_hist[i] > mx) mx = c->tui.rx_bps_hist[i];
    size_t o = 0;
    for (int i = 0; i < ZT_SPARK_HIST; i++) {
        int      idx = (c->tui.rx_bps_head + i) % ZT_SPARK_HIST;
        uint64_t v   = c->tui.rx_bps_hist[idx];
        int      b   = (int)((v * 7) / mx);
        if (b < 0) b = 0;
        if (b > 7) b = 7;
        size_t bl = 3; /* UTF-8 block width */
        if (o + bl + 1 >= cap) break;
        memcpy(out + o, kBlocks[b], bl);
        o += bl;
    }
    out[o] = 0;
    return out;
}
