/**
 * @file    rx_thread.c
 * @brief   Optional single-producer/single-consumer reader thread.
 *
 * When enabled via @c --threaded, a dedicated kernel thread drains the
 * serial device into a lock-free SPSC ring buffer. The main thread reads
 * from that ring and runs the render/log pipeline.
 *
 * This decouples UART interrupt latency from render latency, reducing
 * jitter dramatically during high-baud transmissions. At low baud rates
 * it is strictly a pessimization (extra syscall per byte), so it's opt-in.
 *
 * Concurrency model:
 *   - The ring is a power-of-two SPSC buffer with release/acquire atomics.
 *   - The worker owns a private @c dup(2) of the serial fd, so the main
 *     thread may close/reopen @c c->serial.fd (autobaud, reconnect) and
 *     even let the OS recycle the fd number without racing the worker's
 *     in-flight @c read(). To pick up a new fd, call @c rx_thread_stop()
 *     then @c rx_thread_start() — the @c rx_thread_suspend/resume helpers
 *     wrap exactly that and preserve the user-intent flag in
 *     @c spsc_enabled (owned by the CLI parser).
 *   - Shutdown: main flips the running flag (release) and clears the worker's
 *     dup (local_fd = -1). The worker's read() is O_NONBLOCK, so it sees the
 *     change within one loop and exits; main then joins and closes the dup
 *     (closing before the join would race the worker's in-flight read()).
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zyterm/internal/loop.h"

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
    _Atomic int    running;  /**< release on stop, acquire in loop */
    _Atomic int    local_fd; /**< private dup of serial fd; owned here */
} spsc_ring_t;

/* Active ctx for rx_thread_embed_reset() — set on a successful start,
 * cleared on stop. Lets the embedded fatal-exit paths (which have no
 * handle to zyterm_main's stack-local zt_ctx) stop the worker. Main-thread
 * access only. See rx_thread_embed_reset(). ZT-033. */
static zt_ctx *g_rx_embed_ctx = NULL;

#if ZT_HAVE_PTHREAD
static void wake_main(zt_ctx *c) {
    if (!c || c->serial.spsc_wake_pipe[1] < 0) return;
    unsigned char w = 1;
    ssize_t       n __attribute__((unused));
    n = write(c->serial.spsc_wake_pipe[1], &w, 1); /* best-effort, non-blocking */
}

static void *rx_thread_main(void *arg) {
    zt_ctx      *c = (zt_ctx *)arg;
    spsc_ring_t *r = (spsc_ring_t *)c->serial.spsc_impl;
    if (!r) return NULL;
    unsigned char tmp[ZT_READ_CHUNK];
    while (atomic_load_explicit(&r->running, memory_order_acquire)) {
        int fd = atomic_load_explicit(&r->local_fd, memory_order_acquire);
        if (fd < 0) {
            /* Main is mid-shutdown or hasn't handed us an fd. The close
             * in rx_thread_stop() interrupts a blocked read; this branch
             * catches the case where running was already cleared before
             * we got back to the top of the loop. */
            usleep(1000);
            continue;
        }
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n > 0) {
            size_t head    = atomic_load_explicit(&r->head, memory_order_relaxed);
            size_t tail    = atomic_load_explicit(&r->tail, memory_order_acquire);
            size_t free_sp = r->cap - (head - tail);
            size_t wr      = ((size_t)n < free_sp) ? (size_t)n : free_sp;
            /* Copy in up to two contiguous spans across the wrap (mirrors the
             * consumer in rx_thread_drain) rather than a per-byte masked store
             * the compiler can't vectorise. */
            size_t off   = head & (r->cap - 1);
            size_t first = (r->cap - off < wr) ? (r->cap - off) : wr;
            memcpy(r->buf + off, tmp, first);
            if (wr > first) memcpy(r->buf + 0, tmp + first, wr - first);
            atomic_store_explicit(&r->head, head + wr, memory_order_release);
            wake_main(c);
        } else if (n == 0) {
            /* EOF on a TTY is rare but possible (modem hangup); back
             * off briefly and let main's poll see POLLHUP and drive
             * reconnect. */
            usleep(2000);
        } else {
            if (errno == EINTR) continue;
            if (errno == EBADF) break; /* main closed the dup; exit cleanly */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Serial fd is O_NONBLOCK; nothing to read right now. */
                usleep(500);
                continue;
            }
            /* EIO / ENXIO / ENODEV — device gone. Wake main so it can
             * see POLLHUP on its end of the same fd and drive the
             * reconnect path. We back off so we don't spin on errors. */
            wake_main(c);
            usleep(50000);
        }
    }
    return NULL;
}
#endif

int rx_thread_start(zt_ctx *c) {
#if ZT_HAVE_PTHREAD
    if (!c) return -1;
    /* Idempotent: spsc_impl is the authoritative "thread is up" marker.
     * spsc_enabled may be set by the CLI parser before the thread is
     * actually created, so checking that here would skip start. */
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
    atomic_init(&r->local_fd, -1);

    /* Private dup of the serial fd — see file-header comment for why. */
    if (c->serial.fd >= 0) {
        int dupfd = fcntl(c->serial.fd, F_DUPFD_CLOEXEC, 3);
        if (dupfd < 0) {
            free(r->buf);
            free(r);
            return -1;
        }
        atomic_store_explicit(&r->local_fd, dupfd, memory_order_release);
    }

    if (pipe2(c->serial.spsc_wake_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        int dupfd = atomic_load_explicit(&r->local_fd, memory_order_relaxed);
        if (dupfd >= 0) close(dupfd);
        free(r->buf);
        free(r);
        return -1;
    }

    c->serial.spsc_impl = r;
    if (pthread_create(&r->thread, NULL, rx_thread_main, c) != 0) {
        c->serial.spsc_impl = NULL;
        close(c->serial.spsc_wake_pipe[0]);
        c->serial.spsc_wake_pipe[0] = -1;
        close(c->serial.spsc_wake_pipe[1]);
        c->serial.spsc_wake_pipe[1] = -1;
        int dupfd                   = atomic_load_explicit(&r->local_fd, memory_order_relaxed);
        if (dupfd >= 0) close(dupfd);
        free(r->buf);
        free(r);
        return -1;
    }
    g_rx_embed_ctx = c;
    return 0;
#else
    (void)c;
    return -1;
#endif
}

void rx_thread_stop(zt_ctx *c) {
#if ZT_HAVE_PTHREAD
    if (!c) return;
    spsc_ring_t *r = (spsc_ring_t *)c->serial.spsc_impl;
    if (!r) return;

    /* Signal stop and hand the worker a dead fd (local_fd = -1) so it drops out
     * of its loop. Its fd is O_NONBLOCK (a shared OFD from the O_NONBLOCK serial
     * fd), so read() never blocks — the worker observes running==0 / local_fd<0
     * within one ~1 ms loop iteration and exits, letting the join return
     * promptly. Close the dup only AFTER the join: closing it while the worker
     * might still be inside read() on that same fd is a benign-but-real
     * close/read data race (ThreadSanitizer flags it); once joined, the worker
     * is gone and the close is race-free. */
    atomic_store_explicit(&r->running, 0, memory_order_release);
    int dupfd = atomic_exchange_explicit(&r->local_fd, -1, memory_order_acq_rel);

    pthread_join(r->thread, NULL);
    if (dupfd >= 0) close(dupfd);

    if (c->serial.spsc_wake_pipe[0] >= 0) {
        close(c->serial.spsc_wake_pipe[0]);
        c->serial.spsc_wake_pipe[0] = -1;
    }
    if (c->serial.spsc_wake_pipe[1] >= 0) {
        close(c->serial.spsc_wake_pipe[1]);
        c->serial.spsc_wake_pipe[1] = -1;
    }
    free(r->buf);
    free(r);
    c->serial.spsc_impl = NULL;
    g_rx_embed_ctx      = NULL;
    /* NOTE: spsc_enabled is user intent (set by --threaded). We must
     * not clear it here, otherwise rx_thread_resume() after a paired
     * suspend would skip the restart. */
#else
    (void)c;
#endif
}

/* Stop an orphaned worker from a fatal/embedded exit path — zt_die(),
 * sig_crash()→host, or zt_embed_reset() — where zyterm_main's normal
 * rx_thread_stop(&c) is skipped by the siglongjmp back to the host. The
 * worker holds a pointer to zyterm_main's stack-local zt_ctx; if it keeps
 * running while the host reuses that stack for the next run it writes
 * through a stale pointer (use-after-free), and its dup fd + 1 MiB ring
 * leak. See ZT-033 / INVARIANTS §4.
 *
 * MAIN-THREAD ONLY: this joins the worker and frees the ring, which are
 * not async-signal-safe — sig_crash() must NOT call it and instead relies
 * on the host invoking zt_embed_reset() before the next zyterm_main().
 * Idempotent: rx_thread_stop() nulls g_rx_embed_ctx and spsc_impl. */
void rx_thread_embed_reset(void) {
    if (g_rx_embed_ctx) rx_thread_stop(g_rx_embed_ctx);
}

/* Self-register the reset above so core's zt_embed_reset() / zt_die() run it
 * without naming this loop-layer symbol (INVARIANTS §8). */
__attribute__((constructor)) static void rx_thread_register_embed_reset(void) {
    zt_register_embed_reset(rx_thread_embed_reset);
}

size_t rx_thread_drain(zt_ctx *c, unsigned char *dst, size_t cap) {
#if ZT_HAVE_PTHREAD
    if (!c || !c->serial.spsc_impl || !dst || cap == 0) return 0;
    spsc_ring_t *r     = (spsc_ring_t *)c->serial.spsc_impl;
    size_t       tail  = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t       head  = atomic_load_explicit(&r->head, memory_order_acquire);
    size_t       avail = head - tail;
    if (avail == 0) return 0;
    size_t rd = avail < cap ? avail : cap;
    /* Copy in up to two contiguous spans so we don't loop byte-by-byte
     * across the wrap. */
    size_t off   = tail & (r->cap - 1);
    size_t first = (r->cap - off < rd) ? (r->cap - off) : rd;
    memcpy(dst, r->buf + off, first);
    if (rd > first) memcpy(dst + first, r->buf, rd - first);
    atomic_store_explicit(&r->tail, tail + rd, memory_order_release);
    /* Drain any pending wake bytes so poll() doesn't spin. */
    if (c->serial.spsc_wake_pipe[0] >= 0) {
        unsigned char dump[16];
        while (read(c->serial.spsc_wake_pipe[0], dump, sizeof dump) > 0) {}
    }
    return rd;
#else
    (void)c;
    (void)dst;
    (void)cap;
    return 0;
#endif
}

bool rx_thread_is_running(zt_ctx *c) {
#if ZT_HAVE_PTHREAD
    return c && c->serial.spsc_impl != NULL;
#else
    (void)c;
    return false;
#endif
}

/* Idempotent pause/unpause helpers, intended to bracket c->serial.fd
 * swaps (autobaud, reconnect_attempt, manual reopen). Nesting is safe:
 * pause is a no-op when already stopped, and unpause only restarts if
 * the user actually asked for threaded mode via --threaded. The truth
 * for "should it be running?" lives in c->serial.spsc_enabled. */
void rx_thread_pause(zt_ctx *c) {
    if (rx_thread_is_running(c)) rx_thread_stop(c);
}

void rx_thread_unpause(zt_ctx *c) {
    if (c && c->serial.spsc_enabled && !rx_thread_is_running(c)) (void)rx_thread_start(c);
}
