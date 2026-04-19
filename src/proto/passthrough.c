/**
 * @file passthrough.c
 * @brief Raw KGDB / gdbserver passthrough mode.
 *
 * In passthrough mode zyterm acts as a transparent relay:
 *   - no line editing, no history
 *   - no rendering (raw bytes straight to stdout)
 *   - no macros, no search, no scrollback capture
 *   - stdin bytes → serial verbatim (including Ctrl-C, which is normally
 *     intercepted as ^C → cancel input)
 *
 * This is what you want for `target remote /dev/ttyUSB0` and friends.
 *
 * Exit: the user must detach by sending `~.` (tilde-dot) at the start of
 * a line — same convention as `ssh` and `cu`.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"
#include <unistd.h>

void passthrough_enter(zt_ctx *c) {
    if (!c || c->proto.passthrough) return;
    c->proto.passthrough = true;
    log_notice(c, "passthrough ON — type '~.' at start of line to exit");
}

void passthrough_exit(zt_ctx *c) {
    if (!c || !c->proto.passthrough) return;
    c->proto.passthrough = false;
    log_notice(c, "passthrough OFF");
}

bool passthrough_handle(zt_ctx *c, const unsigned char *buf, size_t n) {
    if (!c || !c->proto.passthrough || !buf || n == 0) return false;
    /* Detect "~." at start of line (after \n or the very first byte). */
    static int state = 0; /* 0 = line start, 1 = saw '~' */
    for (size_t i = 0; i < n; i++) {
        unsigned char b = buf[i];
        if (state == 1 && b == '.') {
            passthrough_exit(c);
            state = 0;
            return true;
        }
        if (state == 0 && b == '~') {
            state = 1;
            continue;
        }
        state = (b == '\n' || b == '\r') ? 0 : 2;
        if (state == 2) state = 0;
    }
    /* Verbatim relay to serial. */
    if (c->serial.fd >= 0) direct_send(c, buf, n);
    return true;
}
