/**
 * @file fuzz_framing.c
 * @brief libFuzzer target for the frame decoders (proto/framing.c).
 *
 * The COBS/SLIP/HDLC/LENPFX decoders consume attacker/device-controlled bytes
 * into a fixed c->proto.buf — exactly the code the ZT-021/ZT-022 bounds/desync
 * fixes touched. This drives framing_feed() with the mode + CRC picked from the
 * first two input bytes and the rest as the wire stream; the decoded-frame sink
 * is a no-op so we're fuzzing the decode/bounds logic in isolation. Build under
 * -fsanitize=fuzzer,address,undefined (see the `fuzz` CI job / `make fuzz`).
 *
 * @author  Iskandar Putra
 * @license MIT
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <stdint.h>
#include <string.h>

static void noop_rx(zt_ctx *c, const unsigned char *b, size_t n) {
    (void)c;
    (void)b;
    (void)n;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;
    zt_ctx c;
    memset(&c, 0, sizeof c);
    c.serial.fd      = -1;
    c.log.fd         = -1;
    c.net.http_fd    = -1;
    c.core.rx_sink   = noop_rx; /* discard decoded frames — no render/scrollback */
    c.proto.mode     = (zt_frame_mode)(data[0] % ZT_FRAME__COUNT);
    c.proto.crc_mode = (zt_crc_mode)(data[1] % ZT_CRC__COUNT);
    /* framing_feed decodes in-place into the ctx (no heap), so no per-run free. */
    framing_feed(&c, data + 2, size - 2);
    return 0;
}
