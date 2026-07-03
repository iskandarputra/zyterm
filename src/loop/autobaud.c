/**
 * @file autobaud.c
 * @brief Brute-force baud-rate probing after a failed reconnect.
 *
 * Iterates over a fixed list of common rates and for each one opens the
 * port briefly, drains what comes in for ~300ms, and scores the result
 * by the fraction of printable ASCII bytes vs total. The highest score
 * wins; ties break by higher baud rate (closer to line rate → usually
 * the intended speed).
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zyterm/internal/loop.h"

#include <poll.h>
#include <string.h>
#include <unistd.h>

static const unsigned kRates[] = {9600,   19200,   38400,   57600,   115200,  230400, 460800,
                                  921600, 1000000, 1500000, 2000000, 3000000, 4000000};

/* Minimum printable-ASCII fraction for a rate to count as a real match.
 * A silent device (total==0) or pure binary at the wrong framing scores
 * ~0; genuine text scores >0.9, while bit-shifted garbage at a wrong baud
 * hovers around the 98/256≈0.38 random-byte floor. 0.5 sits comfortably
 * between them. Below this we report failure so autobaud_probe() doesn't
 * silently lock to the highest rate on a zero-evidence tie (ZT-038). */
#define ZT_AUTOBAUD_MIN_SCORE 0.5

int autobaud_probe(zt_ctx *c) {
    if (!c || !c->serial.device) return -1;
    /* Pause the RX worker so we can swap c->serial.fd without racing
     * its read(). Unpaused on both exit paths below. */
    rx_thread_pause(c);
    int      best_fd    = -1;
    unsigned best_rate  = 0;
    double   best_score = 0.0;
    for (size_t i = 0; i < sizeof kRates / sizeof kRates[0]; i++) {
        int fd = try_reopen_serial(c->serial.device, kRates[i], c->serial.data_bits,
                                   c->serial.parity, c->serial.stop_bits, c->serial.flow);
        if (fd < 0) continue;
        struct pollfd p = {.fd = fd, .events = POLLIN};
        unsigned char buf[1024];
        size_t        total = 0, printable = 0;
        for (int t = 0; t < 3; t++) {
            if (poll(&p, 1, 100) > 0) {
                ssize_t n = read(fd, buf, sizeof buf);
                for (ssize_t j = 0; j < n; j++) {
                    total++;
                    if ((buf[j] >= 0x20 && buf[j] < 0x7F) || buf[j] == '\r' || buf[j] == '\n' ||
                        buf[j] == '\t')
                        printable++;
                }
            }
        }
        double score = (total == 0) ? 0.0 : (double)printable / (double)total;
        if (score > best_score || (score == best_score && kRates[i] > best_rate)) {
            if (best_fd >= 0) close(best_fd);
            best_fd    = fd;
            best_rate  = kRates[i];
            best_score = score;
        } else {
            close(fd);
        }
        set_flash(c, "autobaud: %u  score=%.2f", kRates[i], score);
    }
    /* Reject a no-evidence result: with an idle or all-binary device every
     * rate ties at score 0.0 and the tie-break would otherwise adopt the
     * highest rate as a bogus "match" (ZT-038). Require real printable
     * traffic; on failure keep the caller's default-baud fd untouched (we
     * never assigned c->serial.fd here) so the session stays usable. */
    if (best_fd < 0 || best_score < ZT_AUTOBAUD_MIN_SCORE) {
        if (best_fd >= 0) close(best_fd);
        rx_thread_unpause(c);
        return -1;
    }
    if (c->serial.fd >= 0) close(c->serial.fd);
    c->serial.fd   = best_fd;
    c->serial.baud = best_rate;
    log_notice(c, "autobaud: chose %u bps (score %.2f)", best_rate, best_score);
    rx_thread_unpause(c);
    return 0;
}
