/**
 * @file loglevel.c
 * @brief Per-level volume filter for RX lines.
 *
 * zephyr-style line tags (`<err>`, `<wrn>`, `<inf>`, `<dbg>`) can be
 * muted individually via c->log.mute_dbg / c->log.mute_inf. Returning true
 * from loglevel_muted() tells render.c to discard the line entirely —
 * it never reaches the terminal, scrollback, log, or HTTP stream.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"
#include <string.h>

static const unsigned char *find_tag(const unsigned char *line, size_t len, const char *tag) {
    size_t tl = strlen(tag);
    if (len < tl) return NULL;
    for (size_t i = 0; i + tl <= len; i++)
        if (memcmp(line + i, tag, tl) == 0) return line + i;
    return NULL;
}

bool loglevel_muted(const zt_ctx *c, const unsigned char *line, size_t len) {
    if (!c || !line) return false;
    if (c->log.mute_dbg && find_tag(line, len, "<dbg>")) return true;
    if (c->log.mute_inf && find_tag(line, len, "<inf>")) return true;
    return false;
}
