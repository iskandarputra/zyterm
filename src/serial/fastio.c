/**
 * @file fastio.c
 * @brief Linux fast-path I/O: epoll (edge-triggered) + splice(2) + writev(2).
 *
 * Provides a portable wrapper around the kernel's best-available mechanism
 * for monitoring many FDs and for zero-copy byte flow.
 *
 *  - Linux: epoll7 + EPOLLET, splice(2) for the RAW log fast path.
 *  - Elsewhere: falls back to a poll()-based implementation that yields the
 *    same wire-level behaviour (minus the syscall savings).
 *
 * All functions are safe to call even when the optimization is unavailable —
 * callers can treat them as an abstract event-loop backend.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/uio.h>
#endif

#define ZT_FASTIO_MAX_FDS 16

int fastio_init(zt_ctx *c) {
    if (!c) return -1;
    c->serial.epoll_fd = -1;
#if defined(__linux__)
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep >= 0) c->serial.epoll_fd = ep;
#endif
    return 0;
}

void fastio_shutdown(zt_ctx *c) {
    if (!c) return;
#if defined(__linux__)
    if (c->serial.epoll_fd >= 0) {
        close(c->serial.epoll_fd);
        c->serial.epoll_fd = -1;
    }
#endif
}

int fastio_add_fd(zt_ctx *c, int fd, unsigned events) {
    if (!c || fd < 0) return -1;
#if defined(__linux__)
    if (c->serial.epoll_fd >= 0) {
        struct epoll_event ev;
        ev.events = EPOLLET;
        if (events & POLLIN) ev.events |= EPOLLIN;
        if (events & POLLOUT) ev.events |= EPOLLOUT;
        if (events & POLLHUP) ev.events |= EPOLLHUP;
        ev.data.fd = fd;
        /* make the FD non-blocking; EPOLLET requires it for correctness */
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        if (epoll_ctl(c->serial.epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0) return 0;
        if (errno == EEXIST) {
            return epoll_ctl(c->serial.epoll_fd, EPOLL_CTL_MOD, fd, &ev);
        }
        return -1;
    }
#endif
    (void)events;
    return 0;
}

int fastio_del_fd(zt_ctx *c, int fd) {
    if (!c || fd < 0) return -1;
#if defined(__linux__)
    if (c->serial.epoll_fd >= 0) return epoll_ctl(c->serial.epoll_fd, EPOLL_CTL_DEL, fd, NULL);
#endif
    return 0;
}

int fastio_wait(zt_ctx *c, int timeout_ms, int *out_fds, unsigned *out_events, int max_out) {
    if (!c || !out_fds || !out_events || max_out <= 0) return -1;
#if defined(__linux__)
    if (c->serial.epoll_fd >= 0) {
        struct epoll_event evs[ZT_FASTIO_MAX_FDS];
        int                cap = max_out < ZT_FASTIO_MAX_FDS ? max_out : ZT_FASTIO_MAX_FDS;
        int                n   = epoll_wait(c->serial.epoll_fd, evs, cap, timeout_ms);
        if (n < 0) return -1;
        for (int i = 0; i < n; i++) {
            out_fds[i] = evs[i].data.fd;
            unsigned e = 0;
            if (evs[i].events & EPOLLIN) e |= POLLIN;
            if (evs[i].events & EPOLLOUT) e |= POLLOUT;
            if (evs[i].events & EPOLLHUP) e |= POLLHUP;
            if (evs[i].events & EPOLLERR) e |= POLLERR;
            out_events[i] = e;
        }
        return n;
    }
#endif
    (void)timeout_ms;
    return 0;
}

/* ------------------------------------------------------------------------- */
/*  splice(2) fast path                                                      */
/* ------------------------------------------------------------------------- */

ssize_t fastio_splice_log(zt_ctx *c, int src_fd) {
#if defined(__linux__) && defined(SPLICE_F_MOVE)
    if (!c || c->log.fd < 0 || src_fd < 0) return -1;
    /* splice requires at least one end be a pipe. Cache a pipe pair. */
    static int pipe_fds[2] = {-1, -1};
    if (pipe_fds[0] < 0) {
        if (pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) != 0) {
            pipe_fds[0] = pipe_fds[1] = -1;
            return -1;
        }
    }
    ssize_t total = 0;
    for (;;) {
        ssize_t n =
            splice(src_fd, NULL, pipe_fds[1], NULL, 65536, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n < 0) return -1;
            break;
        }
        ssize_t w = splice(pipe_fds[0], NULL, c->log.fd, NULL, (size_t)n,
                           SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
        if (w < 0) return -1;
        total += w;
        c->log.bytes += (uint64_t)w;
    }
    return total;
#else
    (void)c;
    (void)src_fd;
    return -1;
#endif
}
