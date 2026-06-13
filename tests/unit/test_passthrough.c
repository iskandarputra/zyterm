/**
 * @file test_passthrough.c
 * @brief Transparent (passthrough) mode routing.
 *
 * In passthrough mode zyterm is a transparent relay: device RX is written to the
 * terminal byte-for-byte (no neutralization, no line buffering) and keystrokes go
 * straight to the device; `~.` at line start exits. The on-screen TUI behaviour
 * (scroll-region reset / HUD suspend) is verified manually; this covers the
 * deterministic routing.
 */
#define _GNU_SOURCE 1
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../src/zt_ctx.h"
#include "../../src/zt_internal.h"

static int g_pass, g_fail;

#define ASSERT(cond, msg)                                                                          \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
            fprintf(stderr, "  ok    %s\n", (msg));                                                \
        } else {                                                                                   \
            g_fail++;                                                                              \
            fprintf(stderr, "  FAIL  %s (%s:%d)\n", (msg), __FILE__, __LINE__);                    \
        }                                                                                          \
    } while (0)

#define SECTION(name) fprintf(stderr, "\n=== %s ===\n", (name))

static unsigned char g_rec[4096];
static size_t        g_rec_len;
static void          rec_cb(const unsigned char *b, size_t n) {
    if (g_rec_len + n <= sizeof g_rec) {
        memcpy(g_rec + g_rec_len, b, n);
        g_rec_len += n;
    }
}

static void ctx_min(zt_ctx *c) {
    memset(c, 0, sizeof *c);
    c->serial.fd   = -1;
    c->log.fd      = -1;
    c->net.http_fd = -1;
}

static void test_rx_raw(void) {
    SECTION("passthrough: device RX relayed raw to the terminal");
    zt_ctx c;
    ctx_min(&c);
    c.proto.passthrough = true;

    g_rec_len = 0;
    ob_set_record_callback(rec_cb);
    const char *in = "\x1b[1;33m<wrn> hi\r\n"; /* ESC + CR + LF — all must pass verbatim */
    rx_ingest(&c, (const unsigned char *)in, strlen(in));
    ob_flush();
    ob_set_record_callback(NULL);

    ASSERT(g_rec_len == strlen(in) && memcmp(g_rec, in, g_rec_len) == 0,
           "RX passes through byte-for-byte (no neutralization, no line buffering)");
}

static void test_stdin_relay(void) {
    SECTION("passthrough: keystrokes relayed raw to the device");
    int fds[2];
    if (pipe(fds) != 0) {
        ASSERT(0, "pipe()");
        return;
    }
    zt_ctx c;
    ctx_min(&c);
    c.proto.passthrough = true;
    c.serial.fd         = fds[1];

    handle_stdin_chunk(&c, (const unsigned char *)"ls -la", 6);
    unsigned char buf[32];
    ssize_t       r = read(fds[0], buf, sizeof buf);
    ASSERT(r == 6 && memcmp(buf, "ls -la", 6) == 0, "typed bytes go straight to the serial fd");

    close(fds[0]);
    close(fds[1]);
}

static void test_exit_seq(void) {
    SECTION("passthrough: '~.' at line start exits");
    int fds[2];
    if (pipe(fds) != 0) {
        ASSERT(0, "pipe()");
        return;
    }
    zt_ctx c;
    ctx_min(&c);
    c.proto.passthrough = true;
    c.serial.fd         = fds[1];

    handle_stdin_chunk(&c, (const unsigned char *)"~", 1);
    ASSERT(c.proto.passthrough == true, "lone '~' does not exit yet");
    handle_stdin_chunk(&c, (const unsigned char *)".", 1);
    ASSERT(c.proto.passthrough == false, "'~.' exits passthrough");

    close(fds[0]);
    close(fds[1]);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_rx_raw();
    test_stdin_relay();
    test_exit_seq();

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "%d passed, %d failed\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");
    return g_fail == 0 ? 0 : 1;
}
