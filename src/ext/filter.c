/**
 * @file filter.c
 * @brief External filter subprocess — pipes RX bytes through a shell command.
 *
 * User launches `--filter 'jq -c .'` or similar; we fork a child, set up
 * two pipes (our stdout → child stdin, child stdout → our read), and
 * relay:
 *      device RX  →  filter stdin
 *      filter stdout  →  render / log / http
 *
 * TX bytes bypass the filter entirely. The filter is expected to be
 * line-buffered (set `stdbuf -oL` in the shell command if needed).
 *
 * The main event loop polls filter_poll_fd(c) alongside the serial fd
 * and drains via filter_drain() into render_rx().
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int filter_start(zt_ctx *c, const char *shell_cmd) {
    if (!c || !shell_cmd || c->ext.filter_pid > 0) return -1;
    int in_pipe[2], out_pipe[2];
    /* O_CLOEXEC so the pipe ends don't leak into any other subprocess
     * we might fork later (xmodem, multi-pane shell, ...). dup2() in
     * the child clears CLOEXEC on the new fd. */
    if (pipe2(in_pipe, O_CLOEXEC) != 0) return -1;
    if (pipe2(out_pipe, O_CLOEXEC) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        execl("/bin/sh", "sh", "-c", shell_cmd, (char *)NULL);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    /* Non-blocking on our read end for clean draining. */
    int fl = fcntl(out_pipe[0], F_GETFL, 0);
    fcntl(out_pipe[0], F_SETFL, fl | O_NONBLOCK);

    c->ext.filter_stdin_fd  = in_pipe[1];
    c->ext.filter_stdout_fd = out_pipe[0];
    c->ext.filter_pid       = pid;
    if (c->ext.filter_cmd) free(c->ext.filter_cmd);
    c->ext.filter_cmd = strdup(shell_cmd);
    log_notice(c, "filter started: %s (pid %d)", shell_cmd, (int)pid);
    return 0;
}

void filter_stop(zt_ctx *c) {
    if (!c || c->ext.filter_pid <= 0) return;
    if (c->ext.filter_stdin_fd >= 0) {
        close(c->ext.filter_stdin_fd);
        c->ext.filter_stdin_fd = -1;
    }
    kill(c->ext.filter_pid, SIGTERM);
    int status = 0;
    waitpid(c->ext.filter_pid, &status, 0);
    if (c->ext.filter_stdout_fd >= 0) {
        close(c->ext.filter_stdout_fd);
        c->ext.filter_stdout_fd = -1;
    }
    c->ext.filter_pid = 0;
    if (c->ext.filter_cmd) {
        free(c->ext.filter_cmd);
        c->ext.filter_cmd = NULL;
    }
    log_notice(c, "filter stopped");
}

void filter_feed(zt_ctx *c, const unsigned char *buf, size_t n) {
    if (!c || c->ext.filter_stdin_fd < 0 || !buf || n == 0) return;
    /* Best-effort: drop bytes if the pipe is full rather than blocking the TTY. */
    const unsigned char *p    = buf;
    size_t               left = n;
    while (left > 0) {
        ssize_t w = write(c->ext.filter_stdin_fd, p, left);
        if (w <= 0) {
            if (errno == EAGAIN || errno == EINTR) break;
            filter_stop(c);
            return;
        }
        p += w;
        left -= (size_t)w;
    }
}

int filter_poll_fd(const zt_ctx *c) {
    return c ? c->ext.filter_stdout_fd : -1;
}

void filter_drain(zt_ctx *c) {
    if (!c || c->ext.filter_stdout_fd < 0) return;
    unsigned char buf[4096];
    while (1) {
        ssize_t n = read(c->ext.filter_stdout_fd, buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
            if (n == 0) {
                filter_stop(c);
                break;
            }
            break;
        }
        render_rx(c, buf, (size_t)n);
    }
}
