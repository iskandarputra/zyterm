/**
 * @file session.c
 * @brief Detach/attach over a named UNIX socket.
 *
 * `zyterm --detach NAME` keeps the serial connection alive in a
 * background process, accepts clients on `/tmp/zyterm.<NAME>.sock`,
 * relays RX to each attached client and forwards client-sent bytes to
 * the device.
 *
 * `zyterm --attach NAME` connects to that socket, forwards stdin raw
 * bytes, and prints RX bytes raw. Multiple attached clients see the
 * same multiplexed RX stream.
 *
 * The detached process owns all rendering and line editing; the
 * attach path is a transparent byte pipe.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

static void session_path(const char *name, char *out, size_t cap) {
    snprintf(out, cap, "/tmp/zyterm.%s.sock", name);
}

int session_detach(zt_ctx *c, const char *name) {
    if (!c || !name || c->net.session_fd >= 0) return -1;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    session_path(name, path, sizeof path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = {0};
    addr.sun_family         = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    c->net.session_fd = fd;
    if (c->net.session_name) free(c->net.session_name);
    c->net.session_name = strdup(name);
    log_notice(c, "detached — attach with: zyterm --attach %s", name);
    return 0;
}

#define ATT_MAX 8
static int att_fds[ATT_MAX] = {-1, -1, -1, -1, -1, -1, -1, -1};

void       session_embed_reset(void) {
    /* Close any attached-client fds left dangling from a previous
     * embedded invocation. The detach listening socket itself lives on
     * the zt_ctx (c->net.session_fd) so it gets closed in zyterm_main's
     * cleanup; here we only scrub the static accepted-client table. */
    for (int i = 0; i < ATT_MAX; i++) {
        if (att_fds[i] >= 0) close(att_fds[i]);
        att_fds[i] = -1;
    }
}

void session_tick(zt_ctx *c) {
    if (!c || c->net.session_fd < 0) return;
    while (1) {
        int cfd = accept4(c->net.session_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (cfd < 0) {
            if (errno == EAGAIN) break;
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < ATT_MAX; i++) {
            if (att_fds[i] < 0) {
                att_fds[i] = cfd;
                cfd        = -1;
                break;
            }
        }
        if (cfd >= 0) close(cfd);
    }
    /* Read from each attached client and inject into serial. */
    for (int i = 0; i < ATT_MAX; i++) {
        if (att_fds[i] < 0) continue;
        unsigned char buf[1024];
        ssize_t       n = read(att_fds[i], buf, sizeof buf);
        if (n > 0 && c->serial.fd >= 0)
            direct_send(c, buf, (size_t)n);
        else if (n == 0 || (n < 0 && errno != EAGAIN)) {
            close(att_fds[i]);
            att_fds[i] = -1;
        }
    }
}

int session_attach(const char *name) {
    if (!name) return -1;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    session_path(name, path, sizeof path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = {0};
    addr.sun_family         = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        fprintf(stderr, "zyterm: attach %s: %s\n", name, strerror(errno));
        close(fd);
        return -1;
    }
    struct termios old_t, raw_t;
    tcgetattr(STDIN_FILENO, &old_t);
    raw_t = old_t;
    cfmakeraw(&raw_t);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw_t);

    fprintf(stderr, "[attached to %s — Ctrl-\\ to detach]\r\n", name);

    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int fl2 = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, fl2 | O_NONBLOCK);

    /* Helper: restore both termios and stdin flags, no matter how we exit. */
#define SESSION_CLEANUP()                                                                      \
    do {                                                                                       \
        tcsetattr(STDIN_FILENO, TCSANOW, &old_t);                                              \
        if (fl2 >= 0) fcntl(STDIN_FILENO, F_SETFL, fl2);                                       \
        close(fd);                                                                             \
    } while (0)

    struct pollfd p[2] = {{.fd = fd, .events = POLLIN}, {.fd = STDIN_FILENO, .events = POLLIN}};
    unsigned char buf[4096];
    while (1) {
        int r = poll(p, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (p[0].revents & POLLIN) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n > 0)
                (void)zt_write_all(STDOUT_FILENO, buf, (size_t)n);
            else if (n == 0)
                break;
        }
        if (p[1].revents & POLLIN) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            if (n > 0) {
                /* Ctrl-\ (0x1C) = detach */
                for (ssize_t i = 0; i < n; i++) {
                    if (buf[i] == 0x1C) {
                        fprintf(stderr, "\r\n[detached]\r\n");
                        SESSION_CLEANUP();
                        return 0;
                    }
                }
                (void)zt_write_all(fd, buf, (size_t)n);
            }
        }
    }
    SESSION_CLEANUP();
#undef SESSION_CLEANUP
    return 0;
}
