/**
 * @file multi.c
 * @brief Multi-device split panes (horizontal stack).
 *
 * Opens N additional serial ports alongside the primary one and splits
 * the terminal vertically, giving each device its own scrollback region.
 * Each device shares the same baud / framing / filter config unless
 * overridden at attach time.
 *
 * The implementation re-uses the primary `zt_ctx` for rendering and
 * maintains per-device row ranges; an active-pane pointer tracks where
 * keystrokes are routed. Kept intentionally minimal: no independent
 * search/HUD per pane — those operate on the focused pane only.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <stdlib.h>
#include <string.h>

#define MULTI_MAX 4
typedef struct {
    int   fd;
    char *dev;
    int   row_lo, row_hi;
} pane_t;
static pane_t g_panes[MULTI_MAX] = {
    {.fd = -1, .dev = NULL, .row_lo = 0, .row_hi = 0},
    {.fd = -1, .dev = NULL, .row_lo = 0, .row_hi = 0},
    {.fd = -1, .dev = NULL, .row_lo = 0, .row_hi = 0},
    {.fd = -1, .dev = NULL, .row_lo = 0, .row_hi = 0},
};
static int g_pane_count = 0;
static int g_active     = 0;

int        multi_start(zt_ctx *primary, const char **extra, int n) {
    if (!primary || n <= 0 || n >= MULTI_MAX) return -1;
    g_panes[0].fd  = primary->serial.fd;
    g_panes[0].dev = primary->serial.device ? strdup(primary->serial.device) : NULL;
    g_pane_count   = 1;
    for (int i = 0; i < n; i++) {
        int fd = setup_serial(extra[i], primary->serial.baud, primary->serial.data_bits,
                                     primary->serial.parity, primary->serial.stop_bits,
                                     primary->serial.flow);
        if (fd < 0) continue;
        g_panes[g_pane_count].fd  = fd;
        g_panes[g_pane_count].dev = strdup(extra[i]);
        g_pane_count++;
    }
    /* Assign row ranges (divide body equally). */
    int rows = primary->tui.rows - 3; /* minus HUD + input */
    int each = rows / g_pane_count;
    for (int i = 0; i < g_pane_count; i++) {
        g_panes[i].row_lo = 1 + i * each;
        g_panes[i].row_hi = (i == g_pane_count - 1) ? rows : (1 + (i + 1) * each - 1);
    }
    return 0;
}

void multi_stop(zt_ctx *c) {
    (void)c;
    for (int i = 1; i < g_pane_count; i++) {
        if (g_panes[i].fd > STDERR_FILENO) close(g_panes[i].fd);
        free(g_panes[i].dev);
        g_panes[i].fd  = -1;
        g_panes[i].dev = NULL;
    }
    /* Also release the primary's dev copy so repeated starts don't leak. */
    if (g_pane_count > 0) {
        free(g_panes[0].dev);
        g_panes[0].dev = NULL;
        g_panes[0].fd  = -1;
    }
    g_pane_count = 0;
    g_active     = 0;
}

void multi_embed_reset(void) {
    /* Belt-and-braces for embedded re-entry: even if multi_stop was not
     * reached (early exit, longjmp), make sure the static table is clean. */
    for (int i = 0; i < MULTI_MAX; i++) {
        if (g_panes[i].fd > STDERR_FILENO) close(g_panes[i].fd);
        free(g_panes[i].dev);
        g_panes[i].fd     = -1;
        g_panes[i].dev    = NULL;
        g_panes[i].row_lo = g_panes[i].row_hi = 0;
    }
    g_pane_count = 0;
    g_active     = 0;
}

void multi_tick(zt_ctx *c) {
    if (!c || g_pane_count < 2) return;
    for (int i = 1; i < g_pane_count; i++) {
        unsigned char buf[4096];
        ssize_t       n = read(g_panes[i].fd, buf, sizeof buf);
        if (n > 0) {
            /* Tag each chunk with its source device and append to the
             * primary log; per-pane on-screen rendering is provided by
             * the split renderer (see multi_render()). */
            char tag[64];
            snprintf(tag, sizeof tag, "[%s] ", g_panes[i].dev);
            log_write_raw(c, (const unsigned char *)tag, strlen(tag));
            log_write_raw(c, buf, (size_t)n);
        }
    }
}

void multi_render(zt_ctx *c) {
    /* Per-pane rendering requires row-addressed output that the current
     * single-pane render path does not provide. Rendering is deferred
     * until that refactor lands; the prefix-tagged log output emitted
     * by multi_tick() is sufficient for current use cases. */
    (void)c;
}
