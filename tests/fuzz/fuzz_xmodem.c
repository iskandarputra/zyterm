/**
 * @file fuzz_xmodem.c
 * @brief libFuzzer target for the XMODEM receive state machine (proto/xmodem.c).
 *
 * xmodem_receive() parses attacker/device-controlled bytes off the serial fd
 * into fixed 133-byte block buffers, checks the block-number complement and a
 * CCITT CRC, buffers one block ahead, and trims trailing 0x1A padding on EOT —
 * exactly the kind of length-free, index-heavy parser where an off-by-one bites.
 * This drives it with the fuzz input as the "device" stream.
 *
 * Harness shape: a socketpair stands in for the serial line. All fuzz bytes are
 * written into the peer end which is then half-closed, so every read_byte()
 * either returns a byte immediately or hits EOF immediately — never the 1 s
 * XMODEM timeout, keeping each run fast. The receiver's ACK/NAK/'C' writes go
 * back into the (undrained) peer buffer; the read fd is non-blocking so those
 * writes fail fast on EAGAIN instead of ever blocking the fuzzer. A leading SOH
 * (0x01) is injected so the very first handshake read enters the block parser —
 * the interesting code — rather than spending the run in the 'C' handshake.
 * Output goes to /dev/null (xmodem_receive writes the payload to its path arg;
 * it never ftruncates, so /dev/null is a valid, churn-free sink).
 *
 * Build under -fsanitize=fuzzer,address,undefined (see the `fuzz` CI job /
 * `make fuzz`). A crash/leak/UB fails CI.
 *
 * @author  Iskandar Putra
 * @license MIT
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define XM_SOH 0x01 /* start-of-header for a 128-byte block (private to xmodem.c) */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 0;

    /* Feed SOH + the fuzz stream, then half-close so reads see a clean EOF. */
    unsigned char soh = XM_SOH;
    ssize_t       w   = write(sv[1], &soh, 1);
    (void)w;
    for (size_t off = 0; off < size;) {
        ssize_t n = write(sv[1], data + off, size - off);
        if (n <= 0) break; /* peer buffer full — enough bytes queued to fuzz */
        off += (size_t)n;
    }
    shutdown(sv[1], SHUT_WR);

    /* Non-blocking read end: the receiver's ACK/NAK writes then fail fast on a
     * full buffer instead of blocking the fuzzer (write_all ignores the error). */
    int fl = fcntl(sv[0], F_GETFL, 0);
    fcntl(sv[0], F_SETFL, fl | O_NONBLOCK);

    zt_ctx c;
    memset(&c, 0, sizeof c);
    c.serial.fd   = sv[0];
    c.log.fd      = -1;
    c.net.http_fd = -1;

    xmodem_receive(&c, "/dev/null");

    close(sv[0]);
    close(sv[1]);
    return 0;
}
