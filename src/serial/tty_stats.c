/**
 * @file tty_stats.c
 * @brief Kernel-level TTY statistics (TIOCGICOUNT, TIOCMGET) + TCFLSH.
 *
 * These counters are the single most underused debugging surface in serial
 * development: they tell you the *hardware* is seeing framing / overrun /
 * parity errors or a BREAK condition, before anything reaches userspace.
 * We poll them at HUD cadence and render them in the status row, plus emit
 * a scrollback notice when any counter advances by >=1.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/serial.h>
#endif

/* TIOCM line bits are POSIX on Linux/BSD/macOS */
static const struct {
    unsigned    bit;
    const char *name;
} kLineBits[] = {
#ifdef TIOCM_DTR
    {TIOCM_DTR, "DTR"},
#endif
#ifdef TIOCM_RTS
    {TIOCM_RTS, "RTS"},
#endif
#ifdef TIOCM_CTS
    {TIOCM_CTS, "CTS"},
#endif
#ifdef TIOCM_DSR
    {TIOCM_DSR, "DSR"},
#endif
#ifdef TIOCM_CAR
    {TIOCM_CAR, "DCD"},
#endif
#ifdef TIOCM_RNG
    {TIOCM_RNG, "RI"},
#endif
};

const char *tty_stats_modem_str(unsigned mask, char *buf, size_t cap) {
    if (!buf || cap == 0) return "";
    size_t off = 0;
    for (size_t i = 0; i < sizeof kLineBits / sizeof kLineBits[0]; i++) {
        int on = (mask & kLineBits[i].bit) != 0;
        int w  = snprintf(buf + off, cap - off, "%s%s%s", on ? "\033[1;92m" : "\033[2;90m",
                          kLineBits[i].name, "\033[0m ");
        if (w < 0 || (size_t)w >= cap - off) break;
        off += (size_t)w;
    }
    return buf;
}

void tty_stats_poll(zt_ctx *c) {
    if (!c || c->serial.fd < 0) return;
#if defined(__linux__)
    struct serial_icounter_struct ic;
    if (ioctl(c->serial.fd, TIOCGICOUNT, &ic) == 0) {
        /* Raise a flash the first time anything goes wrong per tick. */
        unsigned new_frame   = (unsigned)ic.frame - c->serial.kern_frame_err;
        unsigned new_overrun = (unsigned)ic.overrun - c->serial.kern_overrun_err;
        unsigned new_parity  = (unsigned)ic.parity - c->serial.kern_parity_err;
        unsigned new_brk     = (unsigned)ic.brk - c->serial.kern_brk;
        unsigned new_bufover = (unsigned)ic.buf_overrun - c->serial.kern_buf_overrun;
        if (new_frame || new_overrun || new_parity || new_brk || new_bufover) {
            set_flash(c, "\xe2\x9a\xa0 kern: frame+%u over+%u par+%u brk+%u bufov+%u",
                      new_frame, new_overrun, new_parity, new_brk, new_bufover);
        }
        c->serial.kern_frame_err   = (unsigned)ic.frame;
        c->serial.kern_overrun_err = (unsigned)ic.overrun;
        c->serial.kern_parity_err  = (unsigned)ic.parity;
        c->serial.kern_brk         = (unsigned)ic.brk;
        c->serial.kern_buf_overrun = (unsigned)ic.buf_overrun;
    }
#endif
    int mstat = 0;
    if (ioctl(c->serial.fd, TIOCMGET, &mstat) == 0) c->serial.modem_lines = (unsigned)mstat;
    now(&c->serial.t_last_stats);
}

void tty_stats_flush(zt_ctx *c) {
    if (!c || c->serial.fd < 0) return;
    /* Flush both RX and TX kernel queues so reconnect/rebooted-device
     * doesn't blast stale bytes into the new session. */
    (void)tcflush(c->serial.fd, TCIOFLUSH);
}
