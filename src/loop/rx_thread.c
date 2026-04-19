/**
 * @file    rx_thread.c
 * @brief   Optional single-producer/single-consumer reader thread.
 *
 * When enabled via @c --threaded, a dedicated kernel thread drains the
 * serial device into a lock-free SPSC ring buffer. The main thread reads
 * from that ring and runs the render/log pipeline.
 *
 * This decouples UART interrupt latency from render latency, reducing
 * jitter dramatically during high-baud transmissions.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 * baud rates it is strictly a pessimization (extra syscall per byte), so
 * it's opt-in.
 *
 * The ring is a classic power-of-two lock-free SPSC buffer with relaxed
 * atomics; wakeups from the reader to the main loop go through a pipe that
 * the event loop `poll()`s on.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <pthread.h>
#define ZT_HAVE_PTHREAD 1
#endif

typedef struct {
#if ZT_HAVE_PTHREAD
    pthread_t thread;
#endif
    atomic_size_t  head; /**< producer */
    atomic_size_t  tail; /**< consumer */
    unsigned char *buf;
    size_t         cap;
    _Atomic int    running;
} spsc_ring_t;

#if ZT_HAVE_PTHREAD
static void *rx_thread_main(void *arg) {
    zt_ctx      *c = (zt_ctx *)arg;
    spsc_ring_t *r = (spsc_ring_t *)c->serial.spsc_impl;
    if (!r) return NULL;
    unsigned char tmp[65536];
    while (atomic_load_explicit(&r->running, memory_order_relaxed)) {
        if (c->serial.fd < 0) {
            usleep(1000);
            continue;
        }
        ssize_t n = read(c->serial.fd, tmp, sizeof tmp);
        if (n > 0) {
            size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
            size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
            size_t free = r->cap - (head - tail);
            size_t wr   = ((size_t)n < free) ? (size_t)n : free;
            for (size_t i = 0; i < wr; i++) {
                r->buf[(head + i) & (r->cap - 1)] = tmp[i];
            }
            atomic_store_explicit(&r->head, head + wr, memory_order_release);
            /* wake main loop */
            unsigned char w = 1;
            if (write(c->serial.spsc_wake_pipe[1], &w, 1) < 0) { /* best-effort */
            }
        } else if (n == 0 || (n < 0 && errno != EINTR)) {
            usleep(2000);
        }
    }
    return NULL;
}
#endif

int rx_thread_start(zt_ctx *c);
int rx_thread_start(zt_ctx *c) {
#if ZT_HAVE_PTHREAD
    if (!c) return -1;
    /* Already started? `spsc_impl` is the authoritative "thread is up"
     * marker. `spsc_enabled` may be set by the CLI parser before the
     * thread is actually created, so checking it here would skip start. */
    if (c->serial.spsc_impl) return 0;
    spsc_ring_t *r = calloc(1, sizeof *r);
    if (!r) return -1;
    r->cap = ZT_SPSC_CAP;
    r->buf = malloc(r->cap);
    if (!r->buf) {
        free(r);
        return -1;
    }
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    atomic_init(&r->running, 1);
    if (pipe2(c->serial.spsc_wake_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        free(r->buf);
        free(r);
        return -1;
    }
    c->serial.spsc_impl    = r;
    c->serial.spsc_enabled = true;
    if (pthread_create(&r->thread, NULL, rx_thread_main, c) != 0) {
        free(r->buf);
        free(r);
        c->serial.spsc_impl    = NULL;
        c->serial.spsc_enabled = false;
        return -1;
    }
    return 0;
#else
    (void)c;
    return -1;
#endif
}

void rx_thread_stop(zt_ctx *c);
void rx_thread_stop(zt_ctx *c) {
#if ZT_HAVE_PTHREAD
    if (!c || !c->serial.spsc_enabled) return;
    spsc_ring_t *r = (spsc_ring_t *)c->serial.spsc_impl;
    if (r) {
        atomic_store_explicit(&r->running, 0, memory_order_relaxed);
        pthread_join(r->thread, NULL);
        free(r->buf);
        free(r);
    }
    if (c->serial.spsc_wake_pipe[0] >= 0) {
        close(c->serial.spsc_wake_pipe[0]);
        c->serial.spsc_wake_pipe[0] = -1;
    }
    if (c->serial.spsc_wake_pipe[1] >= 0) {
        close(c->serial.spsc_wake_pipe[1]);
        c->serial.spsc_wake_pipe[1] = -1;
    }
    c->serial.spsc_impl    = NULL;
    c->serial.spsc_enabled = false;
#else
    (void)c;
#endif
}

/** Drain up to @p cap bytes from the ring. Returns count drained. */
size_t rx_thread_drain(zt_ctx *c, unsigned char *dst, size_t cap);
size_t rx_thread_drain(zt_ctx *c, unsigned char *dst, size_t cap) {
#if ZT_HAVE_PTHREAD
    if (!c || !c->serial.spsc_enabled || !dst || cap == 0) return 0;
    spsc_ring_t *r     = (spsc_ring_t *)c->serial.spsc_impl;
    size_t       tail  = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t       head  = atomic_load_explicit(&r->head, memory_order_acquire);
    size_t       avail = head - tail;
    if (avail == 0) return 0;
    size_t rd = avail < cap ? avail : cap;
    for (size_t i = 0; i < rd; i++)
        dst[i] = r->buf[(tail + i) & (r->cap - 1)];
    atomic_store_explicit(&r->tail, tail + rd, memory_order_release);
    /* consume any pending wake bytes */
    unsigned char dump[16];
    while (read(c->serial.spsc_wake_pipe[0], dump, sizeof dump) > 0) {}
    return rd;
#else
    (void)c;
    (void)dst;
    (void)cap;
    return 0;
#endif
}
