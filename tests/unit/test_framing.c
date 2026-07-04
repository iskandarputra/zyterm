/**
 * @file test_framing.c
 * @brief Round-trip + regression tests for the frame decoders/encoders.
 *
 * COBS / SLIP / HDLC / LENPFX consume untrusted device bytes and had recent
 * bounds/logic fixes (ZT-021 COBS run-marker reserve, ZT-022 zero-length LENPFX
 * dispatch) with no regression guard. These tests exercise encode→decode
 * entirely in-process: framing_send() emits through c->core.tx_direct and
 * framing_feed() delivers decoded frames through c->core.rx_sink, so we point
 * both at capture buffers — no sockets, no render path (the layering seam from
 * the 2026-07 refactor). @author Iskandar Putra @license MIT
 */
#define _GNU_SOURCE 1
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ---- capture sinks (override the wired render_rx / direct_send) ---- */
static unsigned char g_enc[16384];
static size_t        g_enc_len;
static unsigned char g_dec[16384];
static size_t        g_dec_len;
static int           g_dec_frames;

static void          cap_tx(zt_ctx *c, const unsigned char *b, size_t n) {
    (void)c;
    if (g_enc_len + n <= sizeof g_enc) {
        memcpy(g_enc + g_enc_len, b, n);
        g_enc_len += n;
    }
}
static void cap_rx(zt_ctx *c, const unsigned char *b, size_t n) {
    (void)c;
    if (g_dec_len + n <= sizeof g_dec) {
        memcpy(g_dec + g_dec_len, b, n);
        g_dec_len += n;
    }
    g_dec_frames++;
}

static void fr_ctx(zt_ctx *c, zt_frame_mode mode, zt_crc_mode crc, bool crc_append) {
    memset(c, 0, sizeof *c);
    c->serial.fd    = -1;
    c->log.fd       = -1;
    c->net.http_fd  = -1;
    c->log.sb_lines = calloc(ZT_SCROLLBACK_CAP, sizeof(char *));
    loop_wire_sinks(c);           /* wire real sinks first … */
    c->core.tx_direct   = cap_tx; /* … then capture encode/decode */
    c->core.rx_sink     = cap_rx;
    c->proto.mode       = mode;
    c->proto.crc_mode   = crc;
    c->proto.crc_append = crc_append;
    g_enc_len = g_dec_len = 0;
    g_dec_frames          = 0;
}

/* Encode `payload` then decode it back; assert one frame identical to input. */
static void roundtrip(const char *name, zt_frame_mode mode, const unsigned char *payload,
                      size_t plen) {
    zt_ctx c;
    fr_ctx(&c, mode, ZT_CRC_NONE, false);
    int en = framing_send(&c, payload, plen);
    ASSERT(en > 0, name);
    framing_feed(&c, g_enc, g_enc_len);
    ASSERT(g_dec_frames == 1, "exactly one frame decoded");
    ASSERT(g_dec_len == plen && memcmp(g_dec, payload, plen) == 0, "decoded frame == payload");
    free(c.log.sb_lines);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    /* Payload with every framing delimiter/escape byte, so each codec must
     * escape correctly: 0x00 (COBS), 0xC0/0xDB (SLIP), 0x7E/0x7D (HDLC), plus
     * 0xFF and ESC. */
    const unsigned char tricky[] = {'h',  'i',  0x00, 0xC0, 0xDB, 0x7E,
                                    0x7D, 0xFF, 0x1B, 0x00, 'z'};

    SECTION("framing round-trip — all modes, delimiter-heavy payload");
    roundtrip("COBS round-trip", ZT_FRAME_COBS, tricky, sizeof tricky);
    roundtrip("SLIP round-trip", ZT_FRAME_SLIP, tricky, sizeof tricky);
    roundtrip("HDLC round-trip", ZT_FRAME_HDLC, tricky, sizeof tricky);
    roundtrip("LENPFX round-trip", ZT_FRAME_LENPFX, tricky, sizeof tricky);

    SECTION("ZT-021 — COBS run longer than 254 non-zero bytes");
    {
        /* >254 nonzero bytes forces encode_cobs to emit a 0xFF run marker; the
         * old n+2 reserve undercounted it. Assert it still round-trips. */
        unsigned char big[300];
        for (size_t i = 0; i < sizeof big; i++)
            big[i] = (unsigned char)(1 + (i % 254)); /* never 0 */
        zt_ctx c;
        fr_ctx(&c, ZT_FRAME_COBS, ZT_CRC_NONE, false);
        int en = framing_send(&c, big, sizeof big);
        ASSERT(en > (int)sizeof big, "COBS encodes a >254 run with a marker byte");
        framing_feed(&c, g_enc, g_enc_len);
        ASSERT(g_dec_frames == 1 && g_dec_len == sizeof big &&
                   memcmp(g_dec, big, sizeof big) == 0,
               "300-byte COBS run round-trips (ZT-021)");
        free(c.log.sb_lines);
    }

    SECTION("ZT-022 — zero-length LENPFX header must not desync the next frame");
    {
        /* A 0x00 0x00 length header (empty frame) must dispatch on completion
         * and NOT swallow the following frame's first byte. Send an empty then
         * a real payload; assert the real one decodes intact. */
        zt_ctx c;
        fr_ctx(&c, ZT_FRAME_LENPFX, ZT_CRC_NONE, false);
        framing_send(&c, (const unsigned char *)"", 0); /* -> 00 00 */
        size_t after_empty = g_enc_len;
        framing_send(&c, (const unsigned char *)"abc", 3);
        framing_feed(&c, g_enc, g_enc_len);
        ASSERT(after_empty == 2, "empty LENPFX frame encodes to a 2-byte header");
        ASSERT(g_dec_len == 3 && memcmp(g_dec, "abc", 3) == 0,
               "frame after a zero-length header decodes intact (ZT-022)");
        free(c.log.sb_lines);
    }

    SECTION("CRC — append + strip + mismatch detection");
    {
        const unsigned char msg[] = "sensor=42";
        zt_ctx              c;
        fr_ctx(&c, ZT_FRAME_COBS, ZT_CRC_CCITT, true);
        framing_send(&c, msg, sizeof msg - 1);
        framing_feed(&c, g_enc, g_enc_len);
        ASSERT(g_dec_len == sizeof msg - 1 && memcmp(g_dec, msg, sizeof msg - 1) == 0,
               "CCITT CRC appended on send, stripped on decode");
        ASSERT(c.proto.crc_err == 0, "valid CRC -> no crc_err");
        free(c.log.sb_lines);

        /* Corrupt one payload byte in the encoded stream -> CRC must fail. */
        fr_ctx(&c, ZT_FRAME_COBS, ZT_CRC_CCITT, true);
        framing_send(&c, msg, sizeof msg - 1);
        g_enc[2] ^= 0xFF; /* flip a byte inside the COBS-encoded payload */
        framing_feed(&c, g_enc, g_enc_len);
        ASSERT(c.proto.crc_err == 1, "corrupted frame -> crc_err incremented");
        free(c.log.sb_lines);
    }

    SECTION("split across feeds — a frame delivered in two chunks decodes once");
    {
        zt_ctx c;
        fr_ctx(&c, ZT_FRAME_SLIP, ZT_CRC_NONE, false);
        framing_send(&c, tricky, sizeof tricky);
        size_t half = g_enc_len / 2;
        framing_feed(&c, g_enc, half);                    /* first chunk */
        framing_feed(&c, g_enc + half, g_enc_len - half); /* remainder */
        ASSERT(g_dec_frames == 1, "split SLIP frame dispatched exactly once");
        ASSERT(g_dec_len == sizeof tricky && memcmp(g_dec, tricky, sizeof tricky) == 0,
               "split SLIP frame decodes identical to single-feed");
        free(c.log.sb_lines);
    }

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "%d passed, %d failed\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");
    return g_fail == 0 ? 0 : 1;
}
