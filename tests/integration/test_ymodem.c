/**
 * @file test_ymodem.c
 * @brief End-to-end test for the YMODEM sender (src/proto/xmodem.c: ymodem_send).
 *
 * ymodem_send is a single-file YMODEM-lite: a block-0 header carrying
 * "name\0size" then an XMODEM-CRC body in 1K (STX) blocks, ending with EOT and
 * a null block-0 — compatible with lrzsz `rb`. All device I/O funnels through
 * c->serial.fd, so we drive it against a socketpair whose far end a forked child
 * plays as a scripted YMODEM receiver: it validates the block-0 name/size, every
 * 1K block's number/complement/CRC, the EOT + null-block end-of-batch, and the
 * reassembled payload. (ymodem_receive delegates to ZMODEM/lrzsz `rz` and is not
 * unit-testable without that binary, so it is out of scope here.)
 *
 * @author  Iskandar Putra
 * @license MIT
 */
#define _GNU_SOURCE 1
#include <fcntl.h>
#include <libgen.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
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

#define SOH           0x01
#define STX           0x02
#define EOT           0x04
#define ACK           0x06

static int rb(int fd, int ms) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, ms) <= 0) return -1;
    unsigned char b;
    return read(fd, &b, 1) == 1 ? (int)b : -1;
}

/* Read the 132 bytes after a block header's SOH/STX (blk, ~blk, N data, 2 CRC)
 * for a 128-byte block; validate the CRC over the N data bytes. Returns 0 ok. */
static int read_and_check(int fd, unsigned char *blk_inv_data_crc, size_t datalen) {
    size_t total = 2 + datalen + 2;
    for (size_t i = 0; i < total; i++) {
        int x = rb(fd, 3000);
        if (x < 0) return -1;
        blk_inv_data_crc[i] = (unsigned char)x;
    }
    uint32_t crc = crc_compute(ZT_CRC_CCITT, blk_inv_data_crc + 2, datalen);
    uint32_t want =
        ((uint32_t)blk_inv_data_crc[2 + datalen] << 8) | blk_inv_data_crc[2 + datalen + 1];
    return crc == want ? 0 : -2;
}

/* Scripted YMODEM receiver. Returns 0 on a fully-valid transfer whose reassembled
 * payload matches `expected`, else a nonzero stage code. */
static int peer_ymodem_receiver(int fd, const char *name, const unsigned char *expected,
                                size_t len) {
    unsigned char c = 'C';
    if (write(fd, &c, 1) != 1) return 10;

    /* block 0: SOH, 0, 0xFF, 128-byte "name\0size", CRC16 */
    if (rb(fd, 3000) != SOH) return 11;
    unsigned char h[2 + 128 + 2];
    if (read_and_check(fd, h, 128) != 0) return 12;
    if (h[0] != 0x00 || h[1] != 0xFF) return 13;
    const char *hname = (const char *)(h + 2);
    if (strcmp(hname, name) != 0) return 14;
    long long hsize = atoll(hname + strlen(hname) + 1);
    if (hsize != (long long)len) return 15;
    unsigned char a = ACK;
    if (write(fd, &a, 1) != 1) return 16;
    c = 'C';
    if (write(fd, &c, 1) != 1) return 17; /* second 'C' requesting the body */

    /* body: 1K STX blocks, numbered from 1 */
    unsigned char got[1 << 16];
    size_t        glen = 0;
    int           blk  = 1;
    for (;;) {
        int first = rb(fd, 3000);
        if (first == EOT) {
            a = ACK;
            if (write(fd, &a, 1) != 1) return 20;
            break;
        }
        if (first != STX) return 21;
        unsigned char b[2 + 1024 + 2];
        if (read_and_check(fd, b, 1024) != 0) return 22;
        if (b[0] != (blk & 0xFF)) return 23;
        if (b[1] != ((~blk) & 0xFF)) return 24;
        if (glen + 1024 <= sizeof got) {
            memcpy(got + glen, b + 2, 1024);
            glen += 1024;
        }
        a = ACK;
        if (write(fd, &a, 1) != 1) return 25;
        blk++;
    }

    /* end-of-batch: a null block-0 (SOH, 0, 0xFF, 128 zero bytes, CRC) */
    if (rb(fd, 3000) != SOH) return 30;
    unsigned char n[2 + 128 + 2];
    if (read_and_check(fd, n, 128) != 0) return 31;
    if (n[0] != 0x00 || n[1] != 0xFF) return 32;
    a = ACK;
    if (write(fd, &a, 1) != 1) return 33;

    if (glen < len) return 40;
    return memcmp(got, expected, len) == 0 ? 0 : 41;
}

static void on_alarm(int sig) {
    (void)sig;
    static const char m[] = "  FAIL  ymodem_send HUNG\n";
    ssize_t           w   = write(2, m, sizeof m - 1);
    (void)w;
    _exit(1);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGALRM, on_alarm);

    const size_t  LEN = 1500; /* spans two 1K blocks (1024 + 476, padded) */
    unsigned char payload[1500];
    for (size_t i = 0; i < LEN; i++)
        payload[i] = (unsigned char)((i * 73 + 5) & 0xFF);

    SECTION("ymodem_send — block-0 header + 1K blocks validated by a scripted receiver");
    {
        char tmpl[] = "/tmp/zyterm_ym_src_XXXXXX";
        int  sfd    = mkstemp(tmpl);
        ASSERT(sfd >= 0 && write(sfd, payload, LEN) == (ssize_t)LEN, "source file written");
        close(sfd);

        /* basename the sender will put in block 0 */
        char tmpcopy[64];
        snprintf(tmpcopy, sizeof tmpcopy, "%s", tmpl);
        char *name = basename(tmpcopy);

        int   sp[2];
        ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair");
        pid_t pid = fork();
        if (pid == 0) {
            close(sp[0]);
            _exit(peer_ymodem_receiver(sp[1], name, payload, LEN));
        }
        close(sp[1]);

        zt_ctx c;
        memset(&c, 0, sizeof c);
        c.serial.fd = sp[0];
        c.log.fd    = -1;
        alarm(10);
        int rc = ymodem_send(&c, tmpl);
        alarm(0);
        int st = 0;
        waitpid(pid, &st, 0);

        ASSERT(rc == 0, "ymodem_send returns 0");
        ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0,
               "receiver validated block-0 name/size, 1K block framing/CRC, EOT + null block, "
               "payload");
        close(sp[0]);
        unlink(tmpl);
    }

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "%d passed, %d failed\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");
    return g_fail == 0 ? 0 : 1;
}
