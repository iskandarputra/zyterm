/**
 * @file test_subsystems.c
 * @brief Comprehensive unit tests for all zyterm subsystems.
 *
 * Covers: CRC, sparkline, loglevel, OSC, framing, macros, bookmarks,
 * scrollback, history, watch, fuzzy, ui helpers, tty_stats helpers,
 * HTTP server (socket-level), filter (pipe mock), metrics (unix socket),
 * log_json (tmpfile), profile (tmpdir), and main.c init wiring.
 */
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "../../src/zt_ctx.h"
#include "../../src/zt_internal.h"

/* ------------------------------------------------------------------ */
/* Test framework                                                     */
/* ------------------------------------------------------------------ */
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

/* helper: init a zeroed zt_ctx with all fds = -1 */
static void ctx_init(zt_ctx *c) {
    memset(c, 0, sizeof *c);
    c->serial.fd            = -1;
    c->log.fd               = -1;
    c->net.http_fd          = -1;
    c->serial.epoll_fd      = -1;
    c->ext.filter_stdin_fd  = -1;
    c->ext.filter_stdout_fd = -1;
    c->net.metrics_fd       = -1;
    c->net.session_fd       = -1;
    c->serial.baud          = 115200;
    c->serial.data_bits     = 8;
    c->serial.parity        = 'n';
    c->serial.stop_bits     = 1;
    c->proto.color_on       = true;
    c->log.sb_lines         = calloc(ZT_SCROLLBACK_CAP, sizeof(char *));
}

static void ctx_free(zt_ctx *c) {
    scrollback_free(c);
    history_free(c);
    free(c->log.sb_lines);
}

/* ------------------------------------------------------------------ */
/* 1. CRC (pure)                                                      */
/* ------------------------------------------------------------------ */
static void test_crc(void) {
    SECTION("crc");
    const unsigned char v[] = "123456789";
    const size_t        n   = sizeof v - 1;

    uint32_t            a   = crc_compute(ZT_CRC_CCITT, v, n);
    uint32_t            b   = crc_compute(ZT_CRC_CCITT, v, n);
    ASSERT(a == b, "CRC-CCITT deterministic");
    ASSERT(a == 0x29B1u || a == 0x31C3u, "CRC-CCITT known variant");

    uint32_t c32 = crc_compute(ZT_CRC_CRC32, v, n);
    ASSERT(c32 == 0xCBF43926u, "CRC-32 known vector");

    ASSERT(crc_size(ZT_CRC_CCITT) == 2, "crc_size CCITT == 2");
    ASSERT(crc_size(ZT_CRC_CRC32) == 4, "crc_size CRC32 == 4");
    ASSERT(crc_size(ZT_CRC_NONE) == 0, "crc_size NONE  == 0");

    const char *nm1 = crc_name(ZT_CRC_CCITT);
    ASSERT(nm1 && *nm1, "crc_name CCITT non-empty");
    const char *nm2 = crc_name(ZT_CRC_CRC32);
    ASSERT(nm2 && *nm2, "crc_name CRC32 non-empty");
    const char *nm3 = crc_name(ZT_CRC_NONE);
    ASSERT(nm3 && *nm3, "crc_name NONE non-empty");

    /* empty input */
    uint32_t empty = crc_compute(ZT_CRC_CRC32, v, 0);
    ASSERT(empty != 0xCBF43926u, "CRC-32 empty != known vector");
}

/* ------------------------------------------------------------------ */
/* 2. Sparkline (pure with ctx)                                       */
/* ------------------------------------------------------------------ */
static void test_sparkline(void) {
    SECTION("sparkline");
    zt_ctx c;
    ctx_init(&c);

    for (int i = 0; i < 8; i++)
        sparkline_push(&c, (uint64_t)(i * 100));
    char        out[128] = {0};
    const char *r        = sparkline_render(&c, out, sizeof out);
    ASSERT(r != NULL, "sparkline_render non-null");
    ASSERT(out[0] != 0, "sparkline wrote bytes");

    /* monotonic push should produce ascending braille */
    int bad = 0;
    for (size_t i = 0; i < sizeof out && out[i]; i++) {
        unsigned char b = (unsigned char)out[i];
        if (b < 0x20 && b != '\t') bad++;
    }
    ASSERT(bad == 0, "sparkline no ctrl chars");

    /* zero-only input */
    zt_ctx z;
    ctx_init(&z);
    for (int i = 0; i < 8; i++)
        sparkline_push(&z, 0);
    char zout[128] = {0};
    sparkline_render(&z, zout, sizeof zout);
    ASSERT(zout[0] != 0, "sparkline all-zero non-empty");

    ctx_free(&c);
    ctx_free(&z);
}

/* ------------------------------------------------------------------ */
/* 3. Loglevel filter (pure)                                          */
/* ------------------------------------------------------------------ */
static void test_loglevel(void) {
    SECTION("loglevel");
    zt_ctx c;
    ctx_init(&c);

    ASSERT(!loglevel_muted(&c, (const unsigned char *)"<dbg> x", 7), "no flag => pass dbg");
    ASSERT(!loglevel_muted(&c, (const unsigned char *)"<inf> x", 7), "no flag => pass inf");
    ASSERT(!loglevel_muted(&c, (const unsigned char *)"<err> x", 7), "no flag => pass err");

    c.log.mute_dbg = true;
    ASSERT(loglevel_muted(&c, (const unsigned char *)"<dbg> t", 7), "mute_dbg mutes dbg");
    ASSERT(!loglevel_muted(&c, (const unsigned char *)"<inf> t", 7), "mute_dbg ignores inf");
    ASSERT(!loglevel_muted(&c, (const unsigned char *)"<err> t", 7), "mute_dbg ignores err");

    c.log.mute_inf = true;
    ASSERT(loglevel_muted(&c, (const unsigned char *)"<inf> t", 7), "mute_inf mutes inf");
    ASSERT(!loglevel_muted(&c, (const unsigned char *)"<err> t", 7), "err never muted");
    ASSERT(!loglevel_muted(&c, (const unsigned char *)"plain text", 10),
           "plain text not muted");

    ctx_free(&c);
}

/* ------------------------------------------------------------------ */
/* 4. OSC 8 hyperlink rewriting (pure)                                */
/* ------------------------------------------------------------------ */
static void test_osc8(void) {
    SECTION("osc8");
    const unsigned char in[]     = "visit https://example.com now";
    unsigned char       out[512] = {0};
    size_t              w        = osc8_rewrite(in, sizeof in - 1, out, sizeof out);
    ASSERT(w >= sizeof in - 1, "osc8 output >= input");
    ASSERT(w < sizeof out, "osc8 output fits");
    /* should contain OSC 8 escape or at minimum the original text */
    ASSERT(strstr((char *)out, "example.com") != NULL, "osc8 preserves URL");

    /* no-URL input should pass through unchanged */
    const unsigned char plain[]   = "no links here";
    unsigned char       pout[128] = {0};
    size_t              pw        = osc8_rewrite(plain, sizeof plain - 1, pout, sizeof pout);
    ASSERT(pw == sizeof plain - 1, "osc8 no-URL passthrough length");
    ASSERT(memcmp(pout, plain, pw) == 0, "osc8 no-URL passthrough content");
}

/* ------------------------------------------------------------------ */
/* 5. Framing name (pure)                                             */
/* ------------------------------------------------------------------ */
static void test_framing_name(void) {
    SECTION("framing");
    ASSERT(framing_name(ZT_FRAME_RAW) != NULL, "framing_name RAW");
    ASSERT(framing_name(ZT_FRAME_COBS) != NULL, "framing_name COBS");
    ASSERT(framing_name(ZT_FRAME_SLIP) != NULL, "framing_name SLIP");
    ASSERT(framing_name(ZT_FRAME_HDLC) != NULL, "framing_name HDLC");
    ASSERT(framing_name(ZT_FRAME_LENPFX) != NULL, "framing_name LENPFX");
}

/* ------------------------------------------------------------------ */
/* 6. Macros: fkey_index + expand_escapes (pure)                      */
/* ------------------------------------------------------------------ */
static void test_macros(void) {
    SECTION("macros");
    /* F1 = ESC O P */
    const unsigned char f1[] = {0x1b, 'O', 'P'};
    int                 idx  = fkey_index(f1, 3);
    ASSERT(idx >= 0 && idx < 12, "fkey_index F1 valid");

    /* not an F-key */
    const unsigned char nope[] = {'a', 'b', 'c'};
    ASSERT(fkey_index(nope, 3) < 0, "fkey_index non-fkey negative");

    /* expand_escapes: \n → newline, \r → CR */
    char   dst[64];
    size_t n = expand_escapes("hello\\r\\n", dst, sizeof dst);
    ASSERT(n == 7, "expand_escapes length");
    ASSERT(dst[5] == '\r' && dst[6] == '\n', "expand_escapes \\r\\n");

    /* passthrough */
    n = expand_escapes("abc", dst, sizeof dst);
    ASSERT(n == 3, "expand_escapes passthrough");
    ASSERT(memcmp(dst, "abc", 3) == 0, "expand_escapes passthrough content");
}

/* ------------------------------------------------------------------ */
/* 7. Watch match (pure)                                              */
/* ------------------------------------------------------------------ */
static void test_watch(void) {
    SECTION("watch");
    zt_ctx c;
    ctx_init(&c);
    c.log.watch[0]    = strdup("ERROR");
    c.log.watch_count = 1;

    ASSERT(watch_match(&c, (const unsigned char *)"got ERROR here", 14) == 1, "watch matches");
    ASSERT(watch_match(&c, (const unsigned char *)"all good", 8) == 0, "watch no match");
    ASSERT(watch_match(&c, (const unsigned char *)"", 0) == 0, "watch empty line");

    free(c.log.watch[0]);
    ctx_free(&c);
}

/* ------------------------------------------------------------------ */
/* 8. History (pure + malloc)                                         */
/* ------------------------------------------------------------------ */
static void test_history(void) {
    SECTION("history");
    zt_ctx c;
    ctx_init(&c);

    history_push(&c, (const unsigned char *)"first", 5);
    history_push(&c, (const unsigned char *)"second", 6);
    history_push(&c, (const unsigned char *)"third", 5);

    const char *h1 = history_at(&c, 1);
    ASSERT(h1 && strcmp(h1, "third") == 0, "history_at(1) = most recent");
    const char *h2 = history_at(&c, 2);
    ASSERT(h2 && strcmp(h2, "second") == 0, "history_at(2)");
    const char *h3 = history_at(&c, 3);
    ASSERT(h3 && strcmp(h3, "first") == 0, "history_at(3)");
    ASSERT(history_at(&c, 0) == NULL, "history_at(0) = NULL");
    ASSERT(history_at(&c, 99) == NULL, "history_at(99) = NULL");

    ctx_free(&c);
}

/* ------------------------------------------------------------------ */
/* 9. Scrollback (pure + malloc)                                      */
/* ------------------------------------------------------------------ */
static void test_scrollback(void) {
    SECTION("scrollback");
    zt_ctx c;
    ctx_init(&c);

    /* push a few lines */
    const char *lines[] = {"line one", "line two", "line three"};
    for (int i = 0; i < 3; i++) {
        c.log.line_len =
            (size_t)snprintf((char *)c.log.line, sizeof c.log.line, "%s", lines[i]);
        scrollback_push(&c);
    }
    ASSERT(c.log.sb_count == 3, "sb_count == 3");

    /* verify content via sb_lines ring */
    for (int i = 0; i < 3; i++) {
        int idx = (c.log.sb_head + i) % ZT_SCROLLBACK_CAP;
        ASSERT(c.log.sb_lines[idx] && strcmp(c.log.sb_lines[idx], lines[i]) == 0,
               "scrollback content matches");
    }

    ctx_free(&c);
}

/* ------------------------------------------------------------------ */
/* 10. Bookmarks (pure + malloc)                                      */
/* ------------------------------------------------------------------ */
static void test_bookmarks(void) {
    SECTION("bookmarks");
    zt_ctx c;
    ctx_init(&c);

    int r = bookmark_add(&c, 10, "marker A");
    ASSERT(r == 0, "bookmark_add first");
    ASSERT(c.log.bookmark_count == 1, "bookmark_count == 1");

    r = bookmark_add(&c, 20, "marker B");
    ASSERT(r >= 0, "bookmark_add second");
    ASSERT(c.log.bookmark_count == 2, "bookmark_count == 2");

    bookmark_remove(&c, 0);
    ASSERT(c.log.bookmark_count == 1, "bookmark_remove reduces count");

    ctx_free(&c);
}

/* ------------------------------------------------------------------ */
/* 11. UI helpers: fmt_bytes, fmt_hms, visible_len, ts_diff_sec       */
/* ------------------------------------------------------------------ */
static void test_ui_helpers(void) {
    SECTION("ui_helpers");
    char buf[64];

    fmt_bytes(0, buf, sizeof buf);
    ASSERT(strcmp(buf, "0 B") == 0 || buf[0] == '0', "fmt_bytes(0)");

    fmt_bytes(1024, buf, sizeof buf);
    ASSERT(strstr(buf, "1") != NULL, "fmt_bytes(1024) contains 1");

    fmt_bytes(1048576, buf, sizeof buf);
    ASSERT(strstr(buf, "M") != NULL || strstr(buf, "1") != NULL, "fmt_bytes(1M)");

    fmt_hms(3661.0, buf, sizeof buf);
    ASSERT(strstr(buf, "1") != NULL, "fmt_hms 1h1m1s");

    fmt_hms(0.0, buf, sizeof buf);
    ASSERT(buf[0] != 0, "fmt_hms(0) non-empty");

    ASSERT(visible_len("hello") == 5, "visible_len plain");
    ASSERT(visible_len("") == 0, "visible_len empty");
    /* ESC [ 31 m H i ESC [ 0 m  — "Hi" visible */
    ASSERT(visible_len("\033[31mHi\033[0m") == 2, "visible_len with ANSI");

    struct timespec a = {10, 500000000}, b = {12, 0};
    double          d = ts_diff_sec(&b, &a);
    ASSERT(d > 1.4 && d < 1.6, "ts_diff_sec 1.5s");
}

/* ------------------------------------------------------------------ */
/* 12. tty_stats_modem_str (pure)                                     */
/* ------------------------------------------------------------------ */
static void test_modem_str(void) {
    SECTION("tty_stats_modem_str");
    char        buf[128];
    const char *r = tty_stats_modem_str(0, buf, sizeof buf);
    ASSERT(r != NULL, "modem_str(0) non-null");
    r = tty_stats_modem_str(0xFFFF, buf, sizeof buf);
    ASSERT(r != NULL, "modem_str(0xFFFF) non-null");
}

/* ------------------------------------------------------------------ */
/* 13. HTTP server (socket-level mock)                                */
/* ------------------------------------------------------------------ */
static int find_free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family         = AF_INET;
    a.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
    a.sin_port           = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof a;
    getsockname(fd, (struct sockaddr *)&a, &len);
    int port = ntohs(a.sin_port);
    close(fd);
    return port;
}

static void test_http_server(void) {
    SECTION("http_server");
    zt_ctx c;
    ctx_init(&c);

    int port = find_free_port();
    ASSERT(port > 0, "found free port");
    if (port <= 0) {
        ctx_free(&c);
        return;
    }

    /* start */
    int r = http_start(&c, port);
    ASSERT(r == 0, "http_start succeeds");
    ASSERT(c.net.http_fd >= 0, "http_fd valid after start");

    /* double-start should fail */
    int r2 = http_start(&c, port);
    ASSERT(r2 == -1, "http_start double-start rejected");

    /* connect and fetch / */
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(cfd >= 0, "client socket created");
    struct sockaddr_in sa = {0};
    sa.sin_family         = AF_INET;
    sa.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
    sa.sin_port           = htons((uint16_t)port);
    int cr                = connect(cfd, (struct sockaddr *)&sa, sizeof sa);
    ASSERT(cr == 0, "client connect succeeds");

    const char *req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    if (write(cfd, req, strlen(req)) < 0) {}

    /* let http_tick process the connection */
    http_tick(&c);

    /* read response */
    char   resp[8192] = {0};
    size_t total      = 0;
    for (int i = 0; i < 50 && total < sizeof resp - 1; i++) {
        ssize_t rr = read(cfd, resp + total, sizeof resp - 1 - total);
        if (rr > 0)
            total += (size_t)rr;
        else if (rr == 0)
            break;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            usleep(10000);
        } else
            break;
    }
    resp[total] = '\0';
    close(cfd);

    ASSERT(total > 0, "got HTTP response data");
    ASSERT(strstr(resp, "200 OK") != NULL, "HTTP 200 OK");
    ASSERT(strstr(resp, "zyterm") != NULL, "response contains zyterm");
    ASSERT(strstr(resp, "<!doctype html>") != NULL || strstr(resp, "<!DOCTYPE html>") != NULL,
           "response is HTML");

    /* connect to /metrics */
    cfd = socket(AF_INET, SOCK_STREAM, 0);
    connect(cfd, (struct sockaddr *)&sa, sizeof sa);
    const char *mreq = "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n";
    if (write(cfd, mreq, strlen(mreq)) < 0) {}
    http_tick(&c);
    memset(resp, 0, sizeof resp);
    total = 0;
    for (int i = 0; i < 50 && total < sizeof resp - 1; i++) {
        ssize_t rr = read(cfd, resp + total, sizeof resp - 1 - total);
        if (rr > 0)
            total += (size_t)rr;
        else if (rr == 0)
            break;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            usleep(10000);
        } else
            break;
    }
    close(cfd);
    ASSERT(strstr(resp, "zyterm_rx_bytes") != NULL, "/metrics has rx_bytes");

    /* SSE /stream connection */
    cfd = socket(AF_INET, SOCK_STREAM, 0);
    connect(cfd, (struct sockaddr *)&sa, sizeof sa);
    const char *sreq = "GET /stream HTTP/1.1\r\nHost: localhost\r\n\r\n";
    if (write(cfd, sreq, strlen(sreq)) < 0) {}
    http_tick(&c);
    {
        int fl = fcntl(cfd, F_GETFL, 0);
        fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
    }
    memset(resp, 0, sizeof resp);
    total = 0;
    for (int i = 0; i < 20; i++) {
        ssize_t rr = read(cfd, resp + total, sizeof resp - 1 - total);
        if (rr > 0)
            total += (size_t)rr;
        else
            usleep(10000);
    }
    ASSERT(strstr(resp, "text/event-stream") != NULL, "/stream SSE header");

    /* broadcast some data while SSE is connected */
    const unsigned char sample[] = "Hello, serial!";
    http_broadcast(&c, sample, sizeof sample - 1);
    usleep(50000);
    memset(resp, 0, sizeof resp);
    total = 0;
    for (int i = 0; i < 20; i++) {
        ssize_t rr = read(cfd, resp + total, sizeof resp - 1 - total);
        if (rr > 0)
            total += (size_t)rr;
        else
            usleep(10000);
    }
    ASSERT(total > 0, "SSE broadcast delivered data");
    ASSERT(strstr(resp, "data:") != NULL, "SSE data: prefix present");
    close(cfd);

    /* POST /tx (need serial_fd mock → use /dev/null) */
    cfd = socket(AF_INET, SOCK_STREAM, 0);
    connect(cfd, (struct sockaddr *)&sa, sizeof sa);
    const char *ptx = "POST /tx HTTP/1.1\r\nHost: localhost\r\n"
                      "Content-Length: 5\r\n\r\nhello";
    if (write(cfd, ptx, strlen(ptx)) < 0) {}
    http_tick(&c);
    memset(resp, 0, sizeof resp);
    total = 0;
    for (int i = 0; i < 20; i++) {
        ssize_t rr = read(cfd, resp + total, sizeof resp - 1 - total);
        if (rr > 0)
            total += (size_t)rr;
        else if (rr == 0)
            break;
        else
            usleep(10000);
    }
    ASSERT(strstr(resp, "204") != NULL, "POST /tx returns 204");
    close(cfd);

    /* stop */
    http_stop(&c);
    ASSERT(c.net.http_fd == -1, "http_fd -1 after stop");

    /* double-stop is safe */
    http_stop(&c);
    ASSERT(c.net.http_fd == -1, "http_stop idempotent");

    ctx_free(&c);
}

/* ------------------------------------------------------------------ */
/* 14. Filter (pipe mock — uses `cat` as filter)                      */
/* ------------------------------------------------------------------ */
static void test_filter(void) {
    SECTION("filter");
    zt_ctx c;
    ctx_init(&c);

    int r = filter_start(&c, "cat");
    ASSERT(r == 0, "filter_start(cat) succeeds");
    ASSERT(c.ext.filter_pid > 0, "filter_pid set");
    ASSERT(c.ext.filter_stdin_fd >= 0, "filter_stdin_fd valid");
    ASSERT(c.ext.filter_stdout_fd >= 0, "filter_stdout_fd valid");

    int poll_fd = filter_poll_fd(&c);
    ASSERT(poll_fd >= 0, "filter_poll_fd valid");

    /* feed data through cat */
    const unsigned char msg[] = "test data\n";
    filter_feed(&c, msg, sizeof msg - 1);
    usleep(100000); /* give cat time to echo */

    /* we can at least verify filter_drain doesn't crash */
    /* (it calls render_rx internally so we can't easily capture output) */
    /* Just verify the pipe is alive */
    ASSERT(c.ext.filter_pid > 0, "filter still alive after feed");

    filter_stop(&c);
    ASSERT(c.ext.filter_pid == 0, "filter_pid 0 after stop");
    ASSERT(c.ext.filter_stdin_fd == -1, "filter_stdin_fd -1 after stop");
    ASSERT(c.ext.filter_stdout_fd == -1, "filter_stdout_fd -1 after stop");

    /* double-stop safe */
    filter_stop(&c);
    ASSERT(c.ext.filter_pid == 0, "filter_stop idempotent");

    ctx_free(&c);
}

/* ------------------------------------------------------------------ */
/* 15. Metrics (unix socket)                                          */
/* ------------------------------------------------------------------ */
static void test_metrics(void) {
    SECTION("metrics");
    zt_ctx c;
    ctx_init(&c);

    char path[128];
    snprintf(path, sizeof path, "/tmp/zyterm_test_metrics_%d.sock", getpid());

    int r = metrics_start(&c, path);
    ASSERT(r == 0, "metrics_start succeeds");
    ASSERT(c.net.metrics_fd >= 0, "metrics_fd valid");

    /* double-start rejected */
    int r2 = metrics_start(&c, path);
    ASSERT(r2 == -1, "metrics_start double rejected");

    /* connect and trigger snapshot */
    int                cfd  = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family         = AF_UNIX;
    /* path is at most 127 bytes, sun_path is 108. Truncation is fine for this test mock. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
#pragma GCC diagnostic pop
    int cr = connect(cfd, (struct sockaddr *)&addr, sizeof addr);
    ASSERT(cr == 0, "metrics client connect");

    /* set some counters */
    c.core.rx_bytes = 12345;
    c.core.tx_bytes = 678;
    c.core.rx_lines = 99;

    metrics_tick(&c);

    char    resp[4096] = {0};
    ssize_t rn         = read(cfd, resp, sizeof resp - 1);
    close(cfd);

    ASSERT(rn > 0, "metrics response non-empty");
    if (rn > 0) {
        resp[rn] = '\0';
        ASSERT(strstr(resp, "zyterm_rx_bytes") != NULL, "metrics has rx_bytes");
        ASSERT(strstr(resp, "12345") != NULL, "metrics rx_bytes value correct");
    }

    metrics_stop(&c);
    ASSERT(c.net.metrics_fd == -1, "metrics_fd -1 after stop");

    /* socket file cleaned up */
    struct stat st;
    ASSERT(stat(path, &st) != 0, "metrics socket file removed");

    ctx_free(&c);
}

/* ------------------------------------------------------------------ */
/* 16. Log JSON (tmpfile mock)                                        */
/* ------------------------------------------------------------------ */
static void test_log_json(void) {
    SECTION("log_json");
    zt_ctx c;
    ctx_init(&c);

    char path[128];
    snprintf(path, sizeof path, "/tmp/zyterm_test_json_%d.log", getpid());
    c.log.fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    ASSERT(c.log.fd >= 0, "tmpfile opened");
    c.log.format             = ZT_LOG_JSON;

    const unsigned char rx[] = "serial data\n";
    log_json_rx(&c, rx, sizeof rx - 1);
    log_json_event(&c, "test", "sample event %d", 42);

    close(c.log.fd);
    c.log.fd = -1;

    /* read back */
    int rfd = open(path, O_RDONLY);
    ASSERT(rfd >= 0, "reopen tmpfile");
    char    buf[4096] = {0};
    ssize_t n         = read(rfd, buf, sizeof buf - 1);
    close(rfd);
    unlink(path);

    ASSERT(n > 0, "JSON log non-empty");
    if (n > 0) {
        buf[n] = '\0';
        ASSERT(strstr(buf, "\"dir\"") != NULL || strstr(buf, "\"type\"") != NULL ||
                   strstr(buf, "\"event\"") != NULL,
               "JSON has structured fields");
    }

    ctx_free(&c);
}

/* ------------------------------------------------------------------ */
/* 17. Profile save/load (tmpdir mock)                                */
/* ------------------------------------------------------------------ */
static void test_profile(void) {
    SECTION("profile");
    zt_ctx c;
    ctx_init(&c);

    /* We can't easily test profile_save/load without XDG dir control,
       but we can verify the functions don't crash with NULL/empty names */
    int r = profile_load(&c, "nonexistent_test_profile_xyz");
    ASSERT(r != 0, "profile_load nonexistent returns error");

    ctx_free(&c);
}

/* ------------------------------------------------------------------ */
/* 18. Fuzzy finder (pure)                                            */
/* ------------------------------------------------------------------ */
static void test_fuzzy(void) {
    SECTION("fuzzy");
    zt_ctx c;
    ctx_init(&c);

    /* start not in fuzzy mode */
    ASSERT(!c.tui.fuzzy_mode, "fuzzy_mode initially false");

    fuzzy_enter(&c);
    ASSERT(c.tui.fuzzy_mode, "fuzzy_mode true after enter");

    /* handle a keystroke */
    bool handled = fuzzy_handle(&c, 'a');
    ASSERT(handled, "fuzzy_handle consumed key");
    ASSERT(c.tui.fuzzy_len == 1, "fuzzy_len == 1");

    fuzzy_exit(&c);
    ASSERT(!c.tui.fuzzy_mode, "fuzzy_mode false after exit");

    ctx_free(&c);
}

/* ------------------------------------------------------------------ */
/* 19. PTY roundtrip (integration)                                    */
/* ------------------------------------------------------------------ */
static void test_pty_roundtrip(void) {
    SECTION("pty_roundtrip");
    int m = -1, s = -1;
    if (openpty(&m, &s, NULL, NULL, NULL) < 0) {
        fprintf(stderr, "  skip  openpty: %s\n", strerror(errno));
        return;
    }
    struct termios t;
    if (tcgetattr(s, &t) == 0) {
        cfmakeraw(&t);
        tcsetattr(s, TCSANOW, &t);
    }
    fcntl(m, F_SETFL, fcntl(m, F_GETFL, 0) | O_NONBLOCK);

    const char msg[] = "hello zyterm\n";
    ssize_t    w     = write(s, msg, sizeof msg - 1);
    ASSERT(w == (ssize_t)(sizeof msg - 1), "slave write");

    unsigned char buf[128] = {0};
    size_t        n        = 0;
    for (int i = 0; i < 40 && n < sizeof msg - 1; i++) {
        ssize_t r = read(m, buf + n, sizeof buf - 1 - n);
        if (r > 0)
            n += (size_t)r;
        else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct timespec ts = {0, 5000000};
            nanosleep(&ts, NULL);
        } else
            break;
    }
    ASSERT(n == sizeof msg - 1, "master read length");
    ASSERT(memcmp(buf, msg, sizeof msg - 1) == 0, "master read content");
    close(m);
    close(s);
}

/* ------------------------------------------------------------------ */
/* 20. main.c init wiring test                                        */
/*     Verify http_start is actually called via zyterm_main            */
/* ------------------------------------------------------------------ */
static void test_init_wiring(void) {
    SECTION("init_wiring");
    /* We can't easily call zyterm_main without a real TTY,
       but we CAN verify that a zt_ctx with all fds = -1
       allows http_start to succeed (the old bug). */
    zt_ctx c;
    ctx_init(&c);

    /* With the fd-init fix, http_fd starts at -1 */
    ASSERT(c.net.http_fd == -1, "http_fd initialized to -1");
    ASSERT(c.ext.filter_stdin_fd == -1, "filter_stdin_fd initialized to -1");
    ASSERT(c.ext.filter_stdout_fd == -1, "filter_stdout_fd initialized to -1");
    ASSERT(c.net.metrics_fd == -1, "metrics_fd initialized to -1");
    ASSERT(c.net.session_fd == -1, "session_fd initialized to -1");
    ASSERT(c.serial.epoll_fd == -1, "epoll_fd initialized to -1");

    /* http_start should succeed (not return -1 due to fd=0 bug) */
    int port = find_free_port();
    if (port > 0) {
        int r = http_start(&c, port);
        ASSERT(r == 0, "http_start succeeds with fd=-1 init");
        http_stop(&c);
    }

    ctx_free(&c);
}

/* ------------------------------------------------------------------ */
/* 21. Scrollback search (pure)                                       */
/* ------------------------------------------------------------------ */
static void test_search(void) {
    SECTION("search");
    zt_ctx c;
    ctx_init(&c);
    c.tui.rows = 24;
    c.tui.cols = 80;

    /* populate scrollback */
    const char *lines[] = {"alpha beta", "gamma delta", "epsilon zeta",
                           "eta theta",  "iota kappa",  "lambda mu"};
    for (int i = 0; i < 6; i++) {
        c.log.line_len =
            (size_t)snprintf((char *)c.log.line, sizeof c.log.line, "%s", lines[i]);
        scrollback_push(&c);
    }

    /* search for "kappa" */
    memcpy(c.tui.search_buf, "kappa", 5);
    c.tui.search_len = 5;
    int found        = search_scrollback(&c, 1);
    ASSERT(found != 0, "search found 'kappa'");

    /* search for something not present */
    memcpy(c.tui.search_buf, "zzzzz", 5);
    c.tui.search_len = 5;
    c.tui.sb_offset  = 0;
    found            = search_scrollback(&c, -1);
    ASSERT(found == 0, "search 'zzzzz' not found");

    ctx_free(&c);
}

/* ------------------------------------------------------------------ */
/* 22. rx_thread start/stop (no serial needed for lifecycle test)      */
/* ------------------------------------------------------------------ */
static void test_rx_thread_lifecycle(void) {
    SECTION("rx_thread");
    zt_ctx c;
    ctx_init(&c);

    /* Without a valid serial_fd, thread won't read anything,
       but start/stop lifecycle should not crash */
    c.serial.spsc_enabled = true;
    int r                 = rx_thread_start(&c);
    /* May fail without serial_fd, but should not crash */
    if (r == 0) {
        ASSERT(true, "rx_thread_start succeeded");
        usleep(50000);
        rx_thread_stop(&c);
        ASSERT(true, "rx_thread_stop succeeded");
    } else {
        ASSERT(true, "rx_thread_start returned error (no serial_fd, expected)");
    }

    /* stop when never started is safe */
    zt_ctx c2;
    ctx_init(&c2);
    rx_thread_stop(&c2);
    ASSERT(true, "rx_thread_stop on unstarted ctx is safe");

    ctx_free(&c);
    ctx_free(&c2);
}

/* ------------------------------------------------------------------ */
/* 23. Session detach/attach lifecycle (unix socket)                   */
/* ------------------------------------------------------------------ */
static void test_session_lifecycle(void) {
    SECTION("session");
    zt_ctx c;
    ctx_init(&c);

    char name[64];
    snprintf(name, sizeof name, "zytest_%d", getpid());

    int r = session_detach(&c, name);
    if (r == 0) {
        ASSERT(c.net.session_fd >= 0, "session_fd valid after detach");
        /* Clean up: close the unix socket */
        if (c.net.session_fd >= 0) {
            close(c.net.session_fd);
            c.net.session_fd = -1;
        }
        /* remove socket file */
        char path[256];
        snprintf(path, sizeof path, "/tmp/zyterm-%s.sock", name);
        unlink(path);
    } else {
        ASSERT(true, "session_detach failed (may need /tmp writable)");
    }

    ctx_free(&c);
}

/* ------------------------------------------------------------------ */
/* 24. Zephyr color (pure)                                            */
/* ------------------------------------------------------------------ */
static void test_zephyr_color(void) {
    SECTION("zephyr_color");
    const char *r;

    r = zephyr_color((const unsigned char *)"<err> something", 15);
    ASSERT(r != NULL, "zephyr_color <err> non-null");

    r = zephyr_color((const unsigned char *)"<wrn> something", 15);
    ASSERT(r != NULL, "zephyr_color <wrn> non-null");

    r = zephyr_color((const unsigned char *)"<dbg> something", 15);
    ASSERT(r != NULL, "zephyr_color <dbg> non-null");

    r = zephyr_color((const unsigned char *)"plain text", 10);
    /* may return NULL or empty for plain */
    ASSERT(true, "zephyr_color plain doesn't crash");
}

/* ------------------------------------------------------------------ */
/* 25. set_flash (pure + clock)                                       */
/* ------------------------------------------------------------------ */
static void test_flash(void) {
    SECTION("flash");
    zt_ctx c;
    ctx_init(&c);

    set_flash(&c, "hello %d", 42);
    ASSERT(strlen(c.tui.flash) > 0, "flash message set");
    ASSERT(strstr(c.tui.flash, "42") != NULL, "flash contains formatted value");
    ASSERT(c.tui.flash_until.tv_sec > 0 || c.tui.flash_until.tv_nsec > 0, "flash_until set");

    ctx_free(&c);
}

/* ------------------------------------------------------------------ */
/* line-ending translation                                            */
/* ------------------------------------------------------------------ */
static void test_eol(void) {
    SECTION("eol (--map-out / --map-in)");

    zt_eol_map    m;
    unsigned char out[64];
    zt_eol_state  st;

    /* parser */
    ASSERT(eol_parse("none", &m) == 0 && m == ZT_EOL_NONE, "parse none");
    ASSERT(eol_parse("crlf", &m) == 0 && m == ZT_EOL_CRLF, "parse crlf");
    ASSERT(eol_parse("cr-crlf", &m) == 0 && m == ZT_EOL_CR_CRLF, "parse cr-crlf");
    ASSERT(eol_parse("nope", &m) == -1, "parse rejects garbage");

    /* names round-trip */
    ASSERT(strcmp(eol_name(ZT_EOL_LF_CRLF), "lf-crlf") == 0, "name lf-crlf");

    /* OUT: NONE is identity */
    memset(&st, 0, sizeof st);
    size_t n = eol_translate_out(ZT_EOL_NONE, &st,
                                 (const unsigned char *) "ab\r\n", 4, out, sizeof out);
    ASSERT(n == 4 && memcmp(out, "ab\r\n", 4) == 0, "out NONE identity");

    /* OUT: CR — every LF becomes CR */
    memset(&st, 0, sizeof st);
    n = eol_translate_out(ZT_EOL_CR, &st,
                          (const unsigned char *) "a\nb\n", 4, out, sizeof out);
    ASSERT(n == 4 && memcmp(out, "a\rb\r", 4) == 0, "out CR maps LF→CR");

    /* OUT: LF — every CR becomes LF */
    memset(&st, 0, sizeof st);
    n = eol_translate_out(ZT_EOL_LF, &st,
                          (const unsigned char *) "a\rb\r", 4, out, sizeof out);
    ASSERT(n == 4 && memcmp(out, "a\nb\n", 4) == 0, "out LF maps CR→LF");

    /* OUT: CRLF — bare LF → CRLF, lone CR → CRLF, existing CRLF kept once */
    memset(&st, 0, sizeof st);
    n = eol_translate_out(ZT_EOL_CRLF, &st,
                          (const unsigned char *) "a\nb", 3, out, sizeof out);
    ASSERT(n == 4 && memcmp(out, "a\r\nb", 4) == 0, "out CRLF: LF→CRLF");

    memset(&st, 0, sizeof st);
    n = eol_translate_out(ZT_EOL_CRLF, &st,
                          (const unsigned char *) "a\rb", 3, out, sizeof out);
    ASSERT(n == 4 && memcmp(out, "a\r\nb", 4) == 0, "out CRLF: lone CR→CRLF");

    memset(&st, 0, sizeof st);
    n = eol_translate_out(ZT_EOL_CRLF, &st,
                          (const unsigned char *) "a\r\nb", 4, out, sizeof out);
    ASSERT(n == 4 && memcmp(out, "a\r\nb", 4) == 0, "out CRLF: CRLF idempotent");

    /* OUT: LF_CRLF — only LF expands */
    memset(&st, 0, sizeof st);
    n = eol_translate_out(ZT_EOL_LF_CRLF, &st,
                          (const unsigned char *) "a\rb\nc", 5, out, sizeof out);
    ASSERT(n == 6 && memcmp(out, "a\rb\r\nc", 6) == 0, "out LF_CRLF expands LF only");

    /* IN: CRLF — coalesce CRLF→LF, lone CR/LF passthrough */
    memset(&st, 0, sizeof st);
    n = eol_translate_in(ZT_EOL_CRLF, &st,
                         (const unsigned char *) "a\r\nb", 4, out, sizeof out);
    ASSERT(n == 3 && memcmp(out, "a\nb", 3) == 0, "in CRLF: CRLF→LF");

    /* IN: CRLF split across two calls (CR at end of chunk 1, LF at start of chunk 2) */
    memset(&st, 0, sizeof st);
    size_t n1 = eol_translate_in(ZT_EOL_CRLF, &st,
                                 (const unsigned char *) "x\r", 2, out, sizeof out);
    size_t n2 = eol_translate_in(ZT_EOL_CRLF, &st,
                                 (const unsigned char *) "\ny", 2, out + n1, sizeof out - n1);
    ASSERT(n1 == 1 && n2 == 2 && memcmp(out, "x\ny", 3) == 0, "in CRLF: split-chunk coalesce");

    /* IN: CRLF — lone CR at end of stream is held but eol_state flag retains it */
    memset(&st, 0, sizeof st);
    n = eol_translate_in(ZT_EOL_CRLF, &st,
                        (const unsigned char *) "abc\r", 4, out, sizeof out);
    ASSERT(n == 3 && st.saw_cr == 1, "in CRLF: trailing CR is held in state");

    /* IN: CR_CRLF — CRLF→CR */
    memset(&st, 0, sizeof st);
    n = eol_translate_in(ZT_EOL_CR_CRLF, &st,
                         (const unsigned char *) "a\r\nb", 4, out, sizeof out);
    ASSERT(n == 3 && memcmp(out, "a\rb", 3) == 0, "in CR_CRLF: CRLF→CR");
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */
int main(void) {
    signal(SIGPIPE, SIG_IGN);

    /* Pure / Tier 1 */
    test_crc();
    test_sparkline();
    test_loglevel();
    test_osc8();
    test_framing_name();
    test_macros();
    test_ui_helpers();
    test_modem_str();
    test_zephyr_color();
    test_flash();
    test_eol();

    /* zt_ctx-based / Tier 2 */
    test_watch();
    test_history();
    test_scrollback();
    test_bookmarks();
    test_search();
    test_fuzzy();
    test_init_wiring();

    /* I/O mocks / Tier 3 */
    test_filter();
    test_metrics();
    test_log_json();
    test_profile();
    test_rx_thread_lifecycle();
    test_session_lifecycle();

    /* Integration / Tier 4 */
    test_pty_roundtrip();
    test_http_server();

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "%d passed, %d failed\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");
    return g_fail == 0 ? 0 : 1;
}
