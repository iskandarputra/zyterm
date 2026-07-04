/**
 * @file test_xmodem.c
 * @brief End-to-end tests for the XMODEM-CRC engine (src/proto/xmodem.c).
 *
 * The 510-line transfer engine — block framing, complement bytes, CRC-16-CCITT,
 * NAK/ACK/CAN, EOT — was entirely untested. All device I/O funnels through
 * c->serial.fd, so we drive it against a socketpair whose far end a forked child
 * plays as the scripted peer: a receiver for xmodem_send (validates every block)
 * and a sender for xmodem_receive. @author Iskandar Putra @license MIT
 */
#define _GNU_SOURCE 1
#include <fcntl.h>
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
#define EOT           0x04
#define ACK           0x06
#define NAK           0x15
#define CAN           0x18

static int rb(int fd, int ms) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, ms) <= 0) return -1;
    unsigned char b;
    return read(fd, &b, 1) == 1 ? (int)b : -1;
}

/* Scripted receiver for xmodem_send: request CRC mode, validate every block's
 * number/complement/CRC, ACK it, and confirm the reassembled payload matches
 * `expected`. Returns 0 on a fully-valid transfer, nonzero (a stage code) else.
 * If `nak_block1` is set, NAK block 1 once to exercise the retransmit path. */
static int peer_receiver(int fd, const unsigned char *expected, size_t len, int nak_block1) {
    unsigned char c = 'C';
    if (write(fd, &c, 1) != 1) return 10;
    unsigned char recv[16384];
    size_t        rlen  = 0;
    int           blk   = 1;
    int           naked = 0;
    for (;;) {
        int b = rb(fd, 3000);
        if (b == EOT) {
            unsigned char a = ACK;
            if (write(fd, &a, 1) != 1) return 11;
            break;
        }
        if (b != SOH) return 12;
        unsigned char f[132];
        for (int i = 0; i < 132; i++) {
            int x = rb(fd, 3000);
            if (x < 0) return 13;
            f[i] = (unsigned char)x;
        }
        if (nak_block1 && blk == 1 && !naked) {
            naked            = 1;
            unsigned char nk = NAK; /* force the sender to resend block 1 */
            if (write(fd, &nk, 1) != 1) return 14;
            continue;
        }
        if (f[0] != (blk & 0xFF)) return 20;
        if (f[1] != ((~blk) & 0xFF)) return 21;
        uint32_t crc  = crc_compute(ZT_CRC_CCITT, f + 2, 128);
        uint32_t want = ((uint32_t)f[130] << 8) | f[131];
        if (crc != want) return 22;
        if (rlen + 128 <= sizeof recv) {
            memcpy(recv + rlen, f + 2, 128);
            rlen += 128;
        }
        unsigned char a = ACK;
        if (write(fd, &a, 1) != 1) return 15;
        blk = (blk + 1) & 0xFF;
    }
    if (rlen < len) return 30;
    return memcmp(recv, expected, len) == 0 ? 0 : 31;
}

/* Scripted sender for xmodem_receive: wait for 'C', send `data` as CRC blocks
 * (0x1A-padded final block), then EOT. Returns 0 on success. */
static int peer_sender(int fd, const unsigned char *data, size_t len) {
    int b;
    do {
        b = rb(fd, 3000);
        if (b < 0) return 1;
    } while (b != 'C');
    int    blk = 1;
    size_t off = 0;
    while (off < len) {
        unsigned char block[133];
        block[0] = SOH;
        block[1] = (unsigned char)(blk & 0xFF);
        block[2] = (unsigned char)(~blk & 0xFF);
        memset(block + 3, 0x1A, 128);
        size_t chunk = len - off < 128 ? len - off : 128;
        memcpy(block + 3, data + off, chunk);
        uint32_t crc = crc_compute(ZT_CRC_CCITT, block + 3, 128);
        block[131]   = (unsigned char)((crc >> 8) & 0xFF);
        block[132]   = (unsigned char)(crc & 0xFF);
        if (write(fd, block, 133) != 133) return 2;
        if (rb(fd, 3000) != ACK) return 3;
        off += chunk;
        blk++;
    }
    unsigned char eot = EOT;
    if (write(fd, &eot, 1) != 1) return 4;
    rb(fd, 3000); /* final ACK */
    return 0;
}

static void fill_pattern(unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        p[i] = (unsigned char)(((i * 37 + 11) % 251) + 1); /* 1..251, never 0x1A-trailing */
    if (n) p[n - 1] = 0x55;                                /* ensure no trailing 0x1A to trim */
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    const size_t  LEN = 300; /* spans 3 XMODEM blocks (128+128+44) */
    unsigned char payload[300];
    fill_pattern(payload, LEN);

    SECTION("xmodem_send — 3-block transfer validated by a scripted receiver");
    {
        char tmpl[] = "/tmp/zyterm_xm_src_XXXXXX";
        int  sfd    = mkstemp(tmpl);
        ASSERT(sfd >= 0 && write(sfd, payload, LEN) == (ssize_t)LEN, "source file written");
        close(sfd);

        int sp[2];
        ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair");
        pid_t pid = fork();
        if (pid == 0) {
            close(sp[0]);
            _exit(peer_receiver(sp[1], payload, LEN, 0));
        }
        close(sp[1]);
        zt_ctx c;
        memset(&c, 0, sizeof c);
        c.serial.fd = sp[0];
        c.log.fd    = -1;
        int rc      = xmodem_send(&c, tmpl);
        int st      = 0;
        waitpid(pid, &st, 0);
        ASSERT(rc == 0, "xmodem_send returns 0");
        ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0,
               "receiver validated block framing + CRC + reassembled payload");
        close(sp[0]);
        unlink(tmpl);
    }

    SECTION("xmodem_send — recovers from a NAK (block retransmit)");
    {
        char tmpl[] = "/tmp/zyterm_xm_src2_XXXXXX";
        int  sfd    = mkstemp(tmpl);
        if (write(sfd, payload, LEN) != (ssize_t)LEN) { /* ignore */
        }
        close(sfd);
        int sp[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
        pid_t pid = fork();
        if (pid == 0) {
            close(sp[0]);
            _exit(peer_receiver(sp[1], payload, LEN, 1)); /* NAK block 1 once */
        }
        close(sp[1]);
        zt_ctx c;
        memset(&c, 0, sizeof c);
        c.serial.fd = sp[0];
        c.log.fd    = -1;
        int rc      = xmodem_send(&c, tmpl);
        int st      = 0;
        waitpid(pid, &st, 0);
        ASSERT(rc == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0,
               "transfer completes + validates after a block-1 NAK/retransmit");
        close(sp[0]);
        unlink(tmpl);
    }

    SECTION("xmodem_send — aborts cleanly when the receiver sends CAN");
    {
        char tmpl[] = "/tmp/zyterm_xm_src3_XXXXXX";
        int  sfd    = mkstemp(tmpl);
        if (write(sfd, payload, LEN) != (ssize_t)LEN) { /* ignore */
        }
        close(sfd);
        int sp[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
        pid_t pid = fork();
        if (pid == 0) {
            close(sp[0]);
            unsigned char c = 'C';
            (void)(write(sp[1], &c, 1) + 1);
            /* Read the first block, then CAN instead of ACK. */
            unsigned char junk[133];
            for (int i = 0; i < 133; i++)
                if (rb(sp[1], 3000) < 0) break;
            (void)junk;
            unsigned char can[2] = {CAN, CAN};
            (void)(write(sp[1], can, 2) + 1);
            usleep(50000);
            _exit(0);
        }
        close(sp[1]);
        zt_ctx c;
        memset(&c, 0, sizeof c);
        c.serial.fd = sp[0];
        c.log.fd    = -1;
        int rc      = xmodem_send(&c, tmpl);
        int st      = 0;
        waitpid(pid, &st, 0);
        ASSERT(rc == -1, "xmodem_send returns -1 on receiver CAN (no hang)");
        close(sp[0]);
        unlink(tmpl);
    }

    SECTION("xmodem_receive — accepts a 3-block transfer from a scripted sender");
    {
        char outtmpl[] = "/tmp/zyterm_xm_out_XXXXXX";
        int  ofd       = mkstemp(outtmpl);
        close(ofd);
        int sp[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
        pid_t pid = fork();
        if (pid == 0) {
            close(sp[0]);
            _exit(peer_sender(sp[1], payload, LEN));
        }
        close(sp[1]);
        zt_ctx c;
        memset(&c, 0, sizeof c);
        c.serial.fd = sp[0];
        c.log.fd    = -1;
        int rc      = xmodem_receive(&c, outtmpl);
        int st      = 0;
        waitpid(pid, &st, 0);
        ASSERT(rc == 0, "xmodem_receive returns 0");
        ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0, "sender completed");

        int           rfd = open(outtmpl, O_RDONLY);
        unsigned char got[512];
        ssize_t       gn = rfd >= 0 ? read(rfd, got, sizeof got) : -1;
        if (rfd >= 0) close(rfd);
        ASSERT(gn == (ssize_t)LEN,
               "received file is the exact payload length (0x1A padding trimmed)");
        ASSERT(gn == (ssize_t)LEN && memcmp(got, payload, LEN) == 0,
               "received bytes == sent payload");
        close(sp[0]);
        unlink(outtmpl);
    }

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "%d passed, %d failed\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");
    return g_fail == 0 ? 0 : 1;
}
