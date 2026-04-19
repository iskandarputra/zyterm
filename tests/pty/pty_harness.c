/* pty_harness.c - zyterm unit + pty-loopback tests */
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "../../src/zt_ctx.h"
#include "../../src/zt_internal.h"

static int g_pass = 0, g_fail = 0;

#define ASSERT(cond, msg)                                                                      \
    do {                                                                                       \
        if (cond) {                                                                            \
            g_pass++;                                                                          \
            fprintf(stderr, "  ok    %s\n", msg);                                              \
        } else {                                                                               \
            g_fail++;                                                                          \
            fprintf(stderr, "  FAIL  %s (%s:%d)\n", msg, __FILE__, __LINE__);                  \
        }                                                                                      \
    } while (0)

static void test_pty_roundtrip(void) {
    fprintf(stderr, "test: pty roundtrip\n");
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
            struct timespec ts = {0, 5 * 1000 * 1000};
            nanosleep(&ts, NULL);
        } else
            break;
    }
    ASSERT(n == sizeof msg - 1, "master read length");
    ASSERT(memcmp(buf, msg, sizeof msg - 1) == 0, "master read content");
    close(m);
    close(s);
}

static void test_crc(void) {
    fprintf(stderr, "test: crc\n");
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
    const char *nm = crc_name(ZT_CRC_CCITT);
    ASSERT(nm && *nm, "crc_name non-empty");
}

static void test_sparkline(void) {
    fprintf(stderr, "test: sparkline\n");
    zt_ctx c = {0};
    for (int i = 0; i < 8; i++)
        sparkline_push(&c, (uint64_t)(i * 100));
    char        out[128] = {0};
    const char *r        = sparkline_render(&c, out, sizeof out);
    ASSERT(r != NULL, "sparkline_render non-null");
    ASSERT(out[0] != 0, "sparkline wrote bytes");
    int bad = 0;
    for (size_t i = 0; i < sizeof out && out[i]; i++) {
        unsigned char b = (unsigned char)out[i];
        if (b < 0x20 && b != '\t') bad++;
    }
    ASSERT(bad == 0, "sparkline no ctrl chars");
}

static void test_loglevel(void) {
    fprintf(stderr, "test: loglevel\n");
    zt_ctx c = {0};
    ASSERT(loglevel_muted(&c, (const unsigned char *)"<dbg> x", 7) == false, "no flag => pass");
    c.log.mute_dbg = true;
    ASSERT(loglevel_muted(&c, (const unsigned char *)"<dbg> t", 7) == true, "mute_dbg mutes");
    ASSERT(loglevel_muted(&c, (const unsigned char *)"<inf> o", 7) == false,
           "mute_dbg ignores inf");
    c.log.mute_inf = true;
    ASSERT(loglevel_muted(&c, (const unsigned char *)"<inf> o", 7) == true, "mute_inf mutes");
    ASSERT(loglevel_muted(&c, (const unsigned char *)"<err> b", 7) == false, "err never muted");
}

static void test_osc8(void) {
    fprintf(stderr, "test: osc8\n");
    const unsigned char in[]     = "visit https://example.com now";
    unsigned char       out[256] = {0};
    size_t              w        = osc8_rewrite(in, sizeof in - 1, out, sizeof out);
    ASSERT(w >= sizeof in - 1, "osc8 output >= input");
    ASSERT(w < sizeof out, "osc8 output fits");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_pty_roundtrip();
    test_crc();
    test_sparkline();
    test_loglevel();
    test_osc8();
    fprintf(stderr, "\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
