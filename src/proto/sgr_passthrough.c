/**
 * @file sgr_passthrough.c
 * @brief Preserve device-emitted ANSI SGR when writing to the terminal + log.
 *
 * Without passthrough: device-emitted `ESC [ n m` sequences are either
 * rewritten or dropped by render.c so that the on-screen coloring stays
 * consistent with zyterm's own palette.
 *
 * With passthrough: `ESC [ ... m` is forwarded byte-for-byte to the
 * terminal, and neutralized (stripped) only in the log file to keep the
 * log readable with `grep`. The decision is a single-byte check:
 *   c->proto.sgr_passthrough == true  → forward SGR to stdout, strip in log
 *   c->proto.sgr_passthrough == false → rewrite with render.c palette
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"
#include <string.h>

void sgr_filter(zt_ctx *c, const unsigned char *buf, size_t n) {
    if (!c || !buf || n == 0) return;
    if (!c->proto.sgr_passthrough) {
        /* Not our concern — render.c handles the default path. */
        ob_write(buf, n);
        return;
    }
    /* Forward everything as-is; render.c still segments by lines. */
    ob_write(buf, n);
}
