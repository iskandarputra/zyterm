/**
 * @file test_rx_ring.c
 * @brief Concurrency test for the --threaded SPSC ring (loop/rx_thread.c).
 *
 * test_subsystems only checks rx_thread_start/stop don't crash; the
 * correctness-critical logic — the producer's free-space clamp + wrap and the
 * consumer's two-span drain, all on hand-rolled release/acquire atomics — was
 * unexercised. Here a real worker thread drains a pipe into the ring while the
 * main thread drains the ring, and we assert byte-exact ordering across a wrap
 * and a documented drop-on-overflow. Build/run this under -fsanitize=thread to
 * check the happens-before edges. @author Iskandar Putra @license MIT
 */
#define _GNU_SOURCE 1
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../src/zt_ctx.h"
#include "../../src/zt_internal.h"

static int g_pass, g_fail;
#define ASSERT(cond, msg)                                                                      \
    do {                                                                                       \
        if (cond) {                                                                            \
            g_pass++;                                                                          \
            fprintf(stderr, "  ok    %s\n", (msg));                                            \
        } else {                                                                               \
            g_fail++;                                                                          \
            fprintf(stderr, "  FAIL  %s (%s:%d)\n", (msg), __FILE__, __LINE__);                \
        }                                                                                      \
    } while (0)
#define SECTION(name) fprintf(stderr, "\n=== %s ===\n", (name))

/* Deterministic byte stream so ordering bugs (drops, wrap slips) show up. */
#define PAT(i) ((unsigned char)(((unsigned long long)(i) * 2654435761ull) >> 24))

static void ring_ctx(zt_ctx *c, int rfd) {
    memset(c, 0, sizeof *c);
    c->serial.fd           = rfd;
    c->log.fd              = -1;
    c->serial.spsc_enabled = true;
}

/* Drain up to `want` bytes into out[]; returns how many arrived before timeout. */
static size_t drain_upto(zt_ctx *c, unsigned char *out, size_t want, int timeout_ms) {
    size_t          got = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (got < want) {
        unsigned char tmp[8192];
        size_t        n = rx_thread_drain(c, tmp, sizeof tmp);
        if (n) {
            if (got + n > want) n = want - got;
            memcpy(out + got, tmp, n);
            got += n;
            continue;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double ms = (now.tv_sec - t0.tv_sec) * 1000.0 + (now.tv_nsec - t0.tv_nsec) / 1e6;
        if (ms > timeout_ms) break;
        usleep(500);
    }
    return got;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    SECTION("SPSC ring — byte-exact round-trip through the worker");
    {
        int p[2];
        ASSERT(pipe(p) == 0, "pipe created");
        int fl = fcntl(p[0], F_GETFL, 0);
        fcntl(p[0], F_SETFL, fl | O_NONBLOCK); /* mirror the O_NONBLOCK serial fd */
        zt_ctx c;
        ring_ctx(&c, p[0]);
        ASSERT(rx_thread_start(&c) == 0, "rx_thread_start");

        const size_t   N   = 200000; /* < ring cap; fits without drops */
        unsigned char *src = malloc(N);
        for (size_t i = 0; i < N; i++)
            src[i] = PAT(i);
        /* Write in <=32 KiB chunks; the worker drains the pipe into the ring
         * concurrently, so the blocking writes make progress. */
        size_t w = 0;
        while (w < N) {
            size_t  chunk = N - w > 32768 ? 32768 : N - w;
            ssize_t r     = write(p[1], src + w, chunk);
            if (r > 0) w += (size_t)r;
        }
        unsigned char *dst = malloc(N);
        size_t         got = drain_upto(&c, dst, N, 3000);
        ASSERT(got == N, "drained every byte written");
        ASSERT(memcmp(dst, src, N) == 0, "ring preserved byte order + content");

        rx_thread_stop(&c);
        close(p[0]);
        close(p[1]);
        free(src);
        free(dst);
    }

    SECTION("SPSC ring — wrap: sustained flow past the ring capacity");
    {
        int p[2];
        if (pipe(p) != 0) return 1;
        int fl = fcntl(p[0], F_GETFL, 0);
        fcntl(p[0], F_SETFL, fl | O_NONBLOCK);
        zt_ctx c;
        ring_ctx(&c, p[0]);
        rx_thread_start(&c);

        /* Push > ring cap total in rounds, draining each round, so head/tail
         * advance past ZT_SPSC_CAP and the two-span wrap copy is exercised. */
        const size_t   round  = 150000;
        int            rounds = (int)(ZT_SPSC_CAP / round) + 3; /* guarantees a wrap */
        size_t         seq    = 0;
        int            ok     = 1;
        unsigned char *buf    = malloc(round);
        unsigned char *out    = malloc(round);
        for (int r = 0; r < rounds && ok; r++) {
            for (size_t i = 0; i < round; i++)
                buf[i] = PAT(seq + i);
            size_t w = 0;
            while (w < round) {
                ssize_t k = write(p[1], buf + w, round - w > 32768 ? 32768 : round - w);
                if (k > 0) w += (size_t)k;
            }
            size_t got = drain_upto(&c, out, round, 3000);
            if (got != round || memcmp(out, buf, round) != 0) ok = 0;
            seq += round;
        }
        ASSERT(seq > ZT_SPSC_CAP, "pushed more than one ring capacity (forced a wrap)");
        ASSERT(ok, "every round byte-exact across the ring wrap");

        rx_thread_stop(&c);
        close(p[0]);
        close(p[1]);
        free(buf);
        free(out);
    }

    SECTION("SPSC ring — overflow drops the tail, never corrupts the prefix");
    {
        int p[2];
        if (pipe(p) != 0) return 1;
        int fl = fcntl(p[0], F_GETFL, 0);
        fcntl(p[0], F_SETFL, fl | O_NONBLOCK);
        zt_ctx c;
        ring_ctx(&c, p[0]);
        rx_thread_start(&c);

        /* Flood ~2x the ring with no draining: the producer fills to capacity
         * then drops. The worker still empties the pipe (dropping ring
         * overflow), so our writes complete. */
        const size_t N   = (size_t)ZT_SPSC_CAP * 2;
        size_t       w   = 0;
        size_t       seq = 0;
        while (w < N) {
            unsigned char chunk[32768];
            for (size_t i = 0; i < sizeof chunk; i++)
                chunk[i] = PAT(seq + i);
            ssize_t k = write(p[1], chunk, sizeof chunk);
            if (k > 0) {
                w += (size_t)k;
                seq += (size_t)k;
            }
        }
        usleep(100000); /* let the worker finish reading + dropping */

        unsigned char *out = malloc(ZT_SPSC_CAP);
        size_t         got = drain_upto(&c, out, ZT_SPSC_CAP, 2000);
        ASSERT(got <= (size_t)ZT_SPSC_CAP, "buffered no more than one ring capacity");
        ASSERT(got >= (size_t)ZT_SPSC_CAP - 65536, "buffered close to a full ring");
        int prefix_ok = 1;
        for (size_t i = 0; i < got; i++)
            if (out[i] != PAT(i)) {
                prefix_ok = 0;
                break;
            }
        ASSERT(prefix_ok,
               "drained bytes are a correct contiguous prefix (drop-tail, no corruption)");
        ASSERT(atomic_load_explicit(&c.serial.spsc_dropped, memory_order_relaxed) > 0,
               "overflow is accounted in spsc_dropped (not a silent drop)");

        rx_thread_stop(&c);
        close(p[0]);
        close(p[1]);
        free(out);
    }

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "%d passed, %d failed\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");
    return g_fail == 0 ? 0 : 1;
}
