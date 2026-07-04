/**
 * @file test_http_parser.c
 * @brief HTTP request-pump edge tests: oversized-header 431 + slowloris non-block.
 *
 * Drives the real bridge (http_start + http_tick over a loopback socket) rather
 * than reaching into the static header pump, so it exercises the production
 * accept → hc_pump_new → classify_request path. Two properties that the socket
 * round-trip tests in test_xmodem/e2e don't cover:
 *
 *   431 — a header block that never terminates and exceeds HC_REQ_CAP (4096) is
 *         answered "431 Request Header Fields Too Large" and the slot closed,
 *         instead of the buffer overrunning or the connection hanging.
 *   slowloris — a client that sends a partial header and then stalls must not
 *         block http_tick(); the loop returns immediately (the drop happens
 *         later, on the HC_HEADER_TIMEOUT_S deadline). We assert the tick is
 *         non-blocking rather than waiting the full 5 s.
 *
 * @author  Iskandar Putra
 * @license MIT
 */
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
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

/* Pick a free loopback TCP port. */
static int free_port(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family         = AF_INET;
    a.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
    a.sin_port           = 0;
    socklen_t l          = sizeof a;
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 ||
        getsockname(s, (struct sockaddr *)&a, &l) != 0) {
        close(s);
        return -1;
    }
    int port = ntohs(a.sin_port);
    close(s);
    return port;
}

static int connect_client(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family         = AF_INET;
    a.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
    a.sin_port           = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Pump http_tick for up to `ms`, reading any client response into buf. Stops
 * early once `need` bytes have been read. Returns bytes read. */
static size_t pump_and_read(zt_ctx *c, int cli, char *buf, size_t cap, int ms, size_t need) {
    size_t got = 0;
    for (int i = 0; i < ms / 5; i++) {
        http_tick(c);
        struct pollfd p = {.fd = cli, .events = POLLIN};
        if (poll(&p, 1, 5) > 0 && (p.revents & POLLIN)) {
            ssize_t r = read(cli, buf + got, cap - 1 - got);
            if (r > 0) {
                got += (size_t)r;
                if (got >= need) break;
            } else if (r == 0) {
                break;
            }
        }
    }
    buf[got] = '\0';
    return got;
}

static double now_sec(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    int port = free_port();
    if (port < 0) {
        fprintf(stderr, "  SKIP  could not allocate a port\n");
        return 0;
    }

    zt_ctx c;
    memset(&c, 0, sizeof c);
    c.serial.fd   = -1;
    c.log.fd      = -1;
    c.net.http_fd = -1;
    ASSERT(http_start(&c, port) == 0, "http bridge started on a free port");
    if (c.net.http_fd < 0) return 1;

    SECTION("oversized header block → 431, slot closed (no overrun/hang)");
    {
        int cli = connect_client(port);
        ASSERT(cli >= 0, "client connected");
        /* A request line then >HC_REQ_CAP bytes with no terminating blank line. */
        char big[5000];
        int  pfx = snprintf(big, sizeof big, "GET / HTTP/1.1\r\n");
        memset(big + pfx, 'A', sizeof big - (size_t)pfx);
        ssize_t w = write(cli, big, sizeof big);
        (void)w;

        char resp[512];
        pump_and_read(&c, cli, resp, sizeof resp, 2000, 16);
        ASSERT(strstr(resp, "431") != NULL,
               "server answered 431 Request Header Fields Too Large");
        close(cli);
    }

    SECTION("partial header then stall → http_tick stays non-blocking");
    {
        int cli = connect_client(port);
        ASSERT(cli >= 0, "client connected");
        const char *partial = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n"; /* no blank line */
        ssize_t     w       = write(cli, partial, strlen(partial));
        (void)w;
        usleep(20 * 1000); /* let the bytes arrive */

        double t0 = now_sec();
        for (int i = 0; i < 5; i++)
            http_tick(&c); /* pumps the stalled slot */
        double elapsed = now_sec() - t0;
        ASSERT(elapsed < 0.5,
               "http_tick returns immediately on a stalled client (no 5s block)");

        /* No complete response should have been produced for a partial request. */
        char          resp[256];
        struct pollfd p   = {.fd = cli, .events = POLLIN};
        size_t        got = 0;
        if (poll(&p, 1, 50) > 0 && (p.revents & POLLIN)) {
            ssize_t r = read(cli, resp, sizeof resp - 1);
            if (r > 0) got = (size_t)r;
        }
        ASSERT(got == 0, "no response yet for an unterminated request (slot left pending)");
        close(cli);
    }

    http_stop(&c);

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "%d passed, %d failed\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");
    return g_fail == 0 ? 0 : 1;
}
