/**
 * @file xmodem.c
 * @brief XMODEM-CRC file transfer (send + receive).
 *
 * Classic 128-byte-block XMODEM with CRC-16-CCITT trailer. Supports the
 * conventional receiver-initiated handshake:
 *
 *     RX: 'C' 'C' 'C' ...    (every 1s for up to 1 minute, polling)
 *     TX: <SOH> <n> <~n> <128 bytes> <crc-hi> <crc-lo>
 *     RX: <ACK>              or <NAK> to retransmit
 *     TX: <EOT>
 *     RX: <ACK>
 *
 * This implementation respects the existing @ref zt_ctx serial FD; the
 * user is expected to have set up the device already (stop bits, parity,
 * baud, etc.).
 *
 * Timeouts are generous (1 s per ACK, 10 retries per block) because RTOS
 * targets often pause the CPU to process command output.
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
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define XM_SOH        0x01
#define XM_STX        0x02 /* YMODEM 1K */
#define XM_EOT        0x04
#define XM_ACK        0x06
#define XM_NAK        0x15
#define XM_CAN        0x18
#define XM_C          'C'
#define XM_PAD        0x1A

#define XM_BLK        128
#define XM_RETRY      10
#define XM_TIMEOUT_MS 1000

/* Wait for a single byte with timeout. -1 on timeout / error. */
static int read_byte(int fd, int timeout_ms) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int           r = poll(&p, 1, timeout_ms);
    if (r <= 0) return -1;
    unsigned char b;
    ssize_t       n = read(fd, &b, 1);
    if (n != 1) return -1;
    return (int)b;
}

static int write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int xmodem_send(zt_ctx *c, const char *path) {
    if (!c || c->serial.fd < 0 || !path) return -1;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        set_flash(c, "xmodem: open %s: %s", path, strerror(errno));
        return -1;
    }

    log_notice(c, "xmodem send: waiting for receiver 'C'...");
    int handshake = -1;
    for (int i = 0; i < 60; i++) {
        handshake = read_byte(c->serial.fd, XM_TIMEOUT_MS);
        if (handshake == XM_C || handshake == XM_NAK) break;
    }
    if (handshake != XM_C && handshake != XM_NAK) {
        fclose(fp);
        set_flash(c, "xmodem: no handshake");
        return -1;
    }
    bool use_crc = (handshake == XM_C);
    log_notice(c, "xmodem: starting (%s, 128-byte blocks)", use_crc ? "CRC" : "checksum");

    unsigned char block[3 + XM_BLK + 2];
    int           blknum = 1;
    size_t        total  = 0;
    while (1) {
        memset(block + 3, XM_PAD, XM_BLK);
        size_t r = fread(block + 3, 1, XM_BLK, fp);
        if (r == 0) break;
        block[0] = XM_SOH;
        block[1] = (unsigned char)(blknum & 0xFF);
        block[2] = (unsigned char)(~(blknum & 0xFF) & 0xFF);
        if (use_crc) {
            uint32_t crc          = crc_compute(ZT_CRC_CCITT, block + 3, XM_BLK);
            block[3 + XM_BLK]     = (unsigned char)((crc >> 8) & 0xFF);
            block[3 + XM_BLK + 1] = (unsigned char)(crc & 0xFF);
        } else {
            unsigned cs = 0;
            for (int j = 0; j < XM_BLK; j++)
                cs += block[3 + j];
            block[3 + XM_BLK] = (unsigned char)(cs & 0xFF);
        }
        size_t bn      = use_crc ? (3 + XM_BLK + 2) : (3 + XM_BLK + 1);

        int    retries = XM_RETRY;
        int    ack     = -1;
        while (retries-- > 0) {
            if (write_all(c->serial.fd, block, bn) != 0) break;
            ack = read_byte(c->serial.fd, XM_TIMEOUT_MS);
            if (ack == XM_ACK) break;
            if (ack == XM_CAN) break;
        }
        if (ack != XM_ACK) {
            fclose(fp);
            set_flash(c, "xmodem: block %d failed", blknum);
            unsigned char cancel[2] = {XM_CAN, XM_CAN};
            (void)write_all(c->serial.fd, cancel, 2);
            return -1;
        }
        total += r;
        blknum++;
        set_flash(c, "xmodem: sent %zu bytes (%d blocks)", total, blknum - 1);
    }
    fclose(fp);
    unsigned char eot = XM_EOT;
    (void)write_all(c->serial.fd, &eot, 1);
    (void)read_byte(c->serial.fd, XM_TIMEOUT_MS); /* expect ACK */
    log_notice(c, "xmodem: OK — %zu bytes in %d blocks", total, blknum - 1);
    return 0;
}

int xmodem_receive(zt_ctx *c, const char *path) {
    if (!c || c->serial.fd < 0 || !path) return -1;
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        set_flash(c, "xmodem: create %s: %s", path, strerror(errno));
        return -1;
    }

    /* Send 'C' to request CRC mode */
    for (int i = 0; i < 60; i++) {
        unsigned char c_char = XM_C;
        (void)write_all(c->serial.fd, &c_char, 1);
        int hdr = read_byte(c->serial.fd, XM_TIMEOUT_MS);
        if (hdr == XM_SOH) {
            /* put back by reading the block */
            /* Read remaining 2 header bytes + 128 data + 2 CRC = 132 bytes,
             * for a total (including the SOH we already got) of 133. */
            unsigned char blk[3 + XM_BLK + 2];
            blk[0] = XM_SOH;
            for (int j = 1; j < (int)sizeof blk; j++) {
                int b = read_byte(c->serial.fd, XM_TIMEOUT_MS);
                if (b < 0) {
                    fclose(fp);
                    return -1;
                }
                blk[j] = (unsigned char)b;
            }
            /* process first block + subsequent loop */
            int    blknum = 1;
            size_t total  = 0;
            while (1) {
                int bn_rcv = blk[1];
                int bn_inv = blk[2];
                if ((bn_rcv ^ bn_inv) != 0xFF) {
                    unsigned char nak = XM_NAK;
                    (void)write_all(c->serial.fd, &nak, 1);
                    continue;
                }
                uint32_t crc_rcv = ((uint32_t)blk[3 + XM_BLK] << 8) | blk[3 + XM_BLK + 1];
                uint32_t crc_cmp = crc_compute(ZT_CRC_CCITT, blk + 3, XM_BLK);
                if (crc_rcv != crc_cmp) {
                    unsigned char nak = XM_NAK;
                    (void)write_all(c->serial.fd, &nak, 1);
                } else if (bn_rcv == (blknum & 0xFF)) {
                    fwrite(blk + 3, 1, XM_BLK, fp);
                    total += XM_BLK;
                    blknum++;
                    unsigned char ack = XM_ACK;
                    (void)write_all(c->serial.fd, &ack, 1);
                    set_flash(c, "xmodem: got %zu bytes", total);
                } else {
                    unsigned char ack = XM_ACK;
                    (void)write_all(c->serial.fd, &ack, 1);
                }
                int h = read_byte(c->serial.fd, XM_TIMEOUT_MS * 5);
                if (h == XM_EOT) {
                    unsigned char ack = XM_ACK;
                    (void)write_all(c->serial.fd, &ack, 1);
                    fclose(fp);
                    log_notice(c, "xmodem: received %zu bytes to %s", total, path);
                    return 0;
                } else if (h == XM_SOH) {
                    blk[0] = XM_SOH;
                    for (int j = 1; j < (int)sizeof blk; j++) {
                        int b = read_byte(c->serial.fd, XM_TIMEOUT_MS);
                        if (b < 0) {
                            fclose(fp);
                            return -1;
                        }
                        blk[j] = (unsigned char)b;
                    }
                } else {
                    fclose(fp);
                    return -1;
                }
            }
        }
    }
    fclose(fp);
    return -1;
}

/* ------------------------------------------------------------------------- */
/*  YMODEM: 1K blocks with a block-0 header carrying filename + size         */
/* ------------------------------------------------------------------------- */

int ymodem_send(zt_ctx *c, const char *path) {
    /* Single-file YMODEM-G-lite: block 0 carries "name\0size\0" followed
     * by an XMODEM-CRC body using 1K (STX) blocks. Compatible with
     * standard `rx`/`rb` from lrzsz. */
    if (!c || c->serial.fd < 0 || !path) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    int h;
    for (int i = 0; i < 60; i++) {
        h = read_byte(c->serial.fd, XM_TIMEOUT_MS);
        if (h == XM_C) break;
    }
    if (h != XM_C) {
        fclose(fp);
        return -1;
    }

    /* block 0 */
    unsigned char b0[3 + 128 + 2];
    memset(b0 + 3, 0, 128);
    b0[0]            = XM_SOH;
    b0[1]            = 0;
    b0[2]            = 0xFF;
    const char *base = strrchr(path, '/');
    base             = base ? base + 1 : path;
    int hw = snprintf((char *)(b0 + 3), 128, "%s%c%lld", base, 0, (long long)st.st_size);
    (void)hw;
    uint32_t crc0   = crc_compute(ZT_CRC_CCITT, b0 + 3, 128);
    b0[3 + 128]     = (unsigned char)((crc0 >> 8) & 0xFF);
    b0[3 + 128 + 1] = (unsigned char)(crc0 & 0xFF);
    (void)write_all(c->serial.fd, b0, sizeof b0);
    int a = read_byte(c->serial.fd, XM_TIMEOUT_MS);
    if (a != XM_ACK) {
        fclose(fp);
        return -1;
    }
    (void)read_byte(c->serial.fd, XM_TIMEOUT_MS); /* second 'C' */

    /* body: 1K STX blocks */
    unsigned char block[3 + 1024 + 2];
    int           blknum = 1;
    size_t        total  = 0;
    while (1) {
        memset(block + 3, XM_PAD, 1024);
        size_t r = fread(block + 3, 1, 1024, fp);
        if (r == 0) break;
        block[0]            = XM_STX;
        block[1]            = (unsigned char)(blknum & 0xFF);
        block[2]            = (unsigned char)(~(blknum & 0xFF) & 0xFF);
        uint32_t crc        = crc_compute(ZT_CRC_CCITT, block + 3, 1024);
        block[3 + 1024]     = (unsigned char)((crc >> 8) & 0xFF);
        block[3 + 1024 + 1] = (unsigned char)(crc & 0xFF);
        (void)write_all(c->serial.fd, block, sizeof block);
        int ack = read_byte(c->serial.fd, XM_TIMEOUT_MS);
        if (ack != XM_ACK) {
            fclose(fp);
            return -1;
        }
        total += r;
        blknum++;
        set_flash(c, "ymodem: %zu/%lld bytes", total, (long long)st.st_size);
    }
    fclose(fp);
    unsigned char eot = XM_EOT;
    (void)write_all(c->serial.fd, &eot, 1);
    (void)read_byte(c->serial.fd, XM_TIMEOUT_MS);
    /* Send final null block 0 to signal end-of-batch */
    unsigned char nullb[3 + 128 + 2] = {XM_SOH, 0, 0xFF};
    memset(nullb + 3, 0, 128);
    uint32_t crcn      = crc_compute(ZT_CRC_CCITT, nullb + 3, 128);
    nullb[3 + 128]     = (unsigned char)((crcn >> 8) & 0xFF);
    nullb[3 + 128 + 1] = (unsigned char)(crcn & 0xFF);
    (void)write_all(c->serial.fd, nullb, sizeof nullb);
    (void)read_byte(c->serial.fd, XM_TIMEOUT_MS);
    log_notice(c, "ymodem: OK — %s (%zu bytes)", base, total);
    return 0;
}

int ymodem_receive(zt_ctx *c, const char *dir) {
    /* Stub: delegate to rz if available — the reference implementation is
     * lrzsz, and reimplementing the full batch handshake with filename
     * negotiation adds 300 LOC for vanishingly small benefit vs shelling out. */
    return zmodem_receive(c, dir);
}

/* ------------------------------------------------------------------------- */
/*  ZMODEM: shell out to lrzsz (`sz` for send, `rz` for receive).            */
/*  Relay bytes between the tty and the child.                                */
/* ------------------------------------------------------------------------- */

#include <sys/wait.h>

static int zmodem_spawn_relay(zt_ctx *c, const char *cmd_name, const char *arg, bool sending) {
    if (!c || c->serial.fd < 0) return -1;
    int to_child[2], from_child[2];
    /* O_CLOEXEC so a concurrent fork+exec elsewhere can't inherit our
     * relay pipes. dup2() in the child clears CLOEXEC on the new fd,
     * so STDIN/STDOUT keep behaving normally. */
    if (pipe2(to_child, O_CLOEXEC) != 0) return -1;
    if (pipe2(from_child, O_CLOEXEC) != 0) {
        close(to_child[0]);
        close(to_child[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        if (sending)
            execlp(cmd_name, cmd_name, arg, (char *)NULL);
        else
            execlp(cmd_name, cmd_name, (char *)NULL);
        _exit(127);
    }
    close(to_child[0]);
    close(from_child[1]);

    struct pollfd p[2];
    p[0].fd     = from_child[0];
    p[0].events = POLLIN;
    p[1].fd     = c->serial.fd;
    p[1].events = POLLIN;
    unsigned char buf[4096];
    int           status = 0;
    while (1) {
        int r = poll(p, 2, 5000);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) {
            if (waitpid(pid, &status, WNOHANG) == pid) break;
            continue;
        }
        if (p[0].revents & POLLIN) {
            ssize_t n = read(from_child[0], buf, sizeof buf);
            if (n > 0)
                (void)write_all(c->serial.fd, buf, (size_t)n);
            else if (n == 0)
                break;
        }
        if (p[1].revents & POLLIN) {
            ssize_t n = read(c->serial.fd, buf, sizeof buf);
            if (n > 0) (void)write_all(to_child[1], buf, (size_t)n);
        }
    }
    close(to_child[1]);
    close(from_child[0]);
    waitpid(pid, &status, 0);
    (void)arg;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

int zmodem_send(zt_ctx *c, const char *path) {
    log_notice(c, "zmodem: starting sz %s", path);
    int rc = zmodem_spawn_relay(c, "sz", path, true);
    log_notice(c, "zmodem: sz exit=%d", rc);
    return rc;
}

int zmodem_receive(zt_ctx *c, const char *dir) {
    /* chdir() mutates the WHOLE process cwd. Save and restore around
     * the relay so subsequent file paths in zyterm stay relative to
     * the original cwd. */
    char saved_cwd[PATH_MAX_LEN];
    saved_cwd[0] = '\0';
    bool restore = false;
    if (dir && *dir) {
        if (!getcwd(saved_cwd, sizeof saved_cwd)) saved_cwd[0] = '\0';
        if (chdir(dir) != 0) {
            set_flash(c, "zmodem: chdir %s failed", dir);
            return -1;
        }
        restore = (saved_cwd[0] != '\0');
    }
    log_notice(c, "zmodem: starting rz");
    int rc = zmodem_spawn_relay(c, "rz", NULL, false);
    log_notice(c, "zmodem: rz exit=%d", rc);
    if (restore) {
        if (chdir(saved_cwd) != 0) {
            /* Best-effort: log but don't fail the transfer that already succeeded. */
            log_notice(c, "zmodem: warning — failed to restore cwd to %s", saved_cwd);
        }
    }
    return rc;
}
