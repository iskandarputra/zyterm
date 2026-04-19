/**
 * @file metrics.c
 * @brief Prometheus-format metrics exporter over a UNIX socket.
 *
 * Writes a text snapshot every time a reader connects. Metrics are in
 * the form:
 *
 *   # HELP zyterm_rx_bytes_total RX bytes since start
 *   # TYPE zyterm_rx_bytes_total counter
 *   zyterm_rx_bytes_total 12345
 *
 * Scraping is cheap — we only build the payload when a reader accepts,
 * and we don't keep state between scrapes. Compatible with
 * `prometheus-community/socket_exporter` out of the box.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int metrics_start(zt_ctx *c, const char *path) {
    if (!c || !path || c->net.metrics_fd >= 0) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = {0};
    addr.sun_family         = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
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
    c->net.metrics_fd = fd;
    if (c->net.metrics_path) free(c->net.metrics_path);
    c->net.metrics_path = strdup(path);
    log_notice(c, "metrics exporter listening on %s", path);
    return 0;
}

void metrics_stop(zt_ctx *c) {
    if (!c) return;
    if (c->net.metrics_fd >= 0) {
        close(c->net.metrics_fd);
        c->net.metrics_fd = -1;
    }
    if (c->net.metrics_path) {
        unlink(c->net.metrics_path);
        free(c->net.metrics_path);
        c->net.metrics_path = NULL;
    }
}

static void write_snapshot(zt_ctx *c, int cfd) {
    char buf[2048];
    int  n = snprintf(buf, sizeof buf,
                      "# HELP zyterm_rx_bytes_total RX bytes since start\n"
                       "# TYPE zyterm_rx_bytes_total counter\n"
                       "zyterm_rx_bytes_total %llu\n"
                       "# HELP zyterm_tx_bytes_total TX bytes since start\n"
                       "# TYPE zyterm_tx_bytes_total counter\n"
                       "zyterm_tx_bytes_total %llu\n"
                       "# HELP zyterm_rx_lines_total RX lines flushed to scrollback\n"
                       "# TYPE zyterm_rx_lines_total counter\n"
                       "zyterm_rx_lines_total %llu\n"
                       "# HELP zyterm_frame_crc_err_total CRC errors on framed RX\n"
                       "# TYPE zyterm_frame_crc_err_total counter\n"
                       "zyterm_frame_crc_err_total %llu\n"
                       "# HELP zyterm_kern_frame_err_total Kernel-reported framing errors\n"
                       "# TYPE zyterm_kern_frame_err_total counter\n"
                       "zyterm_kern_frame_err_total %llu\n"
                       "# HELP zyterm_kern_overrun_err_total Kernel-reported overruns\n"
                       "# TYPE zyterm_kern_overrun_err_total counter\n"
                       "zyterm_kern_overrun_err_total %llu\n",
                      (unsigned long long)c->core.rx_bytes, (unsigned long long)c->core.tx_bytes,
                      (unsigned long long)c->core.rx_lines, (unsigned long long)c->proto.crc_err,
                      (unsigned long long)c->serial.kern_frame_err,
                      (unsigned long long)c->serial.kern_overrun_err);
    if (n > 0) (void)zt_write_all(cfd, buf, (size_t)n);
}

void metrics_tick(zt_ctx *c) {
    if (!c || c->net.metrics_fd < 0) return;
    while (1) {
        int cfd = accept4(c->net.metrics_fd, NULL, NULL, SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break;
        }
        write_snapshot(c, cfd);
        close(cfd);
    }
}
