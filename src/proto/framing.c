/**
 * @file framing.c
 * @brief Frame-oriented decoders/encoders: COBS, SLIP, HDLC-ish, length-prefix.
 *
 * When @c c->proto.mode is non-@c RAW, raw bytes from the serial port are
 * fed through this module; each completed frame is:
 *   1. CRC-checked (if @c c->proto.crc_mode != NONE) with the trailing N bytes,
 *   2. rendered into scrollback as a single line, showing the decoded
 *      payload (as text or hex depending on current mode),
 *   3. counted, and on mismatch flagged with a red notice.
 *
 * Outgoing frames (Enter in interactive mode, or `Ctrl+A C send`) are
 * encoded with the inverse algorithm and, if the user requested it, a
 * trailing CRC.
 *
 * The implementations here are the "textbook" versions — they match the
 * RFCs exactly and interoperate with every production stack the author has
 * tested against (Zephyr MCUmgr, zerotier SLIP bridges, hand-rolled HDLC).
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* render_rx() is what a legitimate "line arrived from device" triggers —
 * we reuse it so framed output is indistinguishable from raw output
 * downstream (logging, search, JSON, broadcast, etc.). */
extern void render_rx(zt_ctx *c, const unsigned char *buf, size_t n);

const char *framing_name(zt_frame_mode m) {
    switch (m) {
    case ZT_FRAME_COBS: return "cobs";
    case ZT_FRAME_SLIP: return "slip";
    case ZT_FRAME_HDLC: return "hdlc";
    case ZT_FRAME_LENPFX: return "len16";
    default: return "raw";
    }
}

void framing_reset(zt_ctx *c) {
    if (!c) return;
    c->proto.len    = 0;
    c->proto.escape = false;
}

/* ------------------------------------------------------------------------- */
/*  Per-frame dispatch after decode                                          */
/* ------------------------------------------------------------------------- */

static void frame_dispatch(zt_ctx *c) {
    size_t n = c->proto.len;
    if (n == 0) return;
    c->proto.rx_count++;

    size_t csz = crc_size(c->proto.crc_mode);
    if (csz && n > csz) {
        uint32_t want = 0;
        if (csz == 2) {
            want = (uint32_t)c->proto.buf[n - 2] << 8 | c->proto.buf[n - 1];
        } else {
            want = ((uint32_t)c->proto.buf[n - 4] << 24) |
                   ((uint32_t)c->proto.buf[n - 3] << 16) |
                   ((uint32_t)c->proto.buf[n - 2] << 8) | ((uint32_t)c->proto.buf[n - 1]);
        }
        uint32_t got = crc_compute(c->proto.crc_mode, c->proto.buf, n - csz);
        if (got != want) {
            c->proto.crc_err++;
            set_flash(c, "\xe2\x9a\xa0 CRC mismatch on frame #%u (want %08x got %08x)",
                      c->proto.rx_count, want, got);
        }
        n -= csz; /* strip trailing CRC from what we render */
    }

    render_rx(c, c->proto.buf, n);
    c->proto.len = 0;
}

/* ------------------------------------------------------------------------- */
/*  COBS decoder (RFC: Cheshire & Baker 1999)                                */
/* ------------------------------------------------------------------------- */

static void feed_cobs(zt_ctx *c, const unsigned char *buf, size_t n) {
    /* COBS frames are terminated by 0x00. We accumulate until we see a 0,
     * then decode the accumulated block in place. */
    static size_t pending = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char b = buf[i];
        if (b == 0x00) {
            /* decode c->proto.buf[0..pending) into c->proto.buf in place */
            size_t rd = 0, wr = 0;
            while (rd < pending) {
                unsigned char code = c->proto.buf[rd++];
                if (code == 0) break;
                size_t copy = (size_t)code - 1;
                if (rd + copy > pending) {
                    copy = pending - rd;
                }
                for (size_t j = 0; j < copy; j++) {
                    if (wr < sizeof c->proto.buf) c->proto.buf[wr++] = c->proto.buf[rd + j];
                }
                rd += copy;
                if (code < 0xFF && rd < pending) {
                    if (wr < sizeof c->proto.buf) c->proto.buf[wr++] = 0x00;
                }
            }
            c->proto.len = wr;
            pending      = 0;
            frame_dispatch(c);
        } else {
            if (pending < sizeof c->proto.buf) c->proto.buf[pending++] = b;
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  SLIP decoder (RFC 1055)                                                  */
/* ------------------------------------------------------------------------- */

#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

static void feed_slip(zt_ctx *c, const unsigned char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char b = buf[i];
        if (c->proto.escape) {
            c->proto.escape   = false;
            unsigned char out = (b == SLIP_ESC_END)   ? SLIP_END
                                : (b == SLIP_ESC_ESC) ? SLIP_ESC
                                                      : b;
            if (c->proto.len < sizeof c->proto.buf) c->proto.buf[c->proto.len++] = out;
            continue;
        }
        if (b == SLIP_END) {
            if (c->proto.len > 0) frame_dispatch(c);
        } else if (b == SLIP_ESC) {
            c->proto.escape = true;
        } else if (c->proto.len < sizeof c->proto.buf) {
            c->proto.buf[c->proto.len++] = b;
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  HDLC-ish decoder (0x7E flag, 0x7D escape; no bit-stuffing — byte-async). */
/* ------------------------------------------------------------------------- */

#define HDLC_FLAG   0x7E
#define HDLC_ESC    0x7D
#define HDLC_ESCXOR 0x20

static void feed_hdlc(zt_ctx *c, const unsigned char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char b = buf[i];
        if (c->proto.escape) {
            c->proto.escape = false;
            if (c->proto.len < sizeof c->proto.buf)
                c->proto.buf[c->proto.len++] = b ^ HDLC_ESCXOR;
            continue;
        }
        if (b == HDLC_FLAG) {
            if (c->proto.len > 0) frame_dispatch(c);
        } else if (b == HDLC_ESC) {
            c->proto.escape = true;
        } else if (c->proto.len < sizeof c->proto.buf) {
            c->proto.buf[c->proto.len++] = b;
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Length-prefixed: <len16 little-endian><payload>                           */
/* ------------------------------------------------------------------------- */

static void feed_len16(zt_ctx *c, const unsigned char *buf, size_t n) {
    static size_t        need = 0;
    static unsigned char lenb[2];
    static int           have_len = 0;
    for (size_t i = 0; i < n; i++) {
        if (have_len < 2) {
            lenb[have_len++] = buf[i];
            if (have_len == 2) {
                need = (size_t)lenb[0] | ((size_t)lenb[1] << 8);
                if (need > sizeof c->proto.buf) {
                    have_len = 0;
                    need     = 0;
                }
                c->proto.len = 0;
            }
        } else {
            if (c->proto.len < need && c->proto.len < sizeof c->proto.buf)
                c->proto.buf[c->proto.len++] = buf[i];
            if (c->proto.len == need) {
                frame_dispatch(c);
                have_len = 0;
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Public API                                                               */
/* ------------------------------------------------------------------------- */

void framing_feed(zt_ctx *c, const unsigned char *buf, size_t n) {
    if (!c || !buf || n == 0) return;
    switch (c->proto.mode) {
    case ZT_FRAME_COBS: feed_cobs(c, buf, n); break;
    case ZT_FRAME_SLIP: feed_slip(c, buf, n); break;
    case ZT_FRAME_HDLC: feed_hdlc(c, buf, n); break;
    case ZT_FRAME_LENPFX: feed_len16(c, buf, n); break;
    default: render_rx(c, buf, n); break;
    }
}

/* ------------------------------------------------------------------------- */
/*  Encoders                                                                 */
/* ------------------------------------------------------------------------- */

static size_t encode_cobs(const unsigned char *in, size_t n, unsigned char *out, size_t cap) {
    if (cap < n + 2) return 0;
    size_t        wr       = 0;
    size_t        code_pos = wr++;
    unsigned char code     = 1;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == 0) {
            out[code_pos] = code;
            code_pos      = wr++;
            code          = 1;
        } else {
            out[wr++] = in[i];
            if (++code == 0xFF) {
                out[code_pos] = code;
                code_pos      = wr++;
                code          = 1;
            }
        }
    }
    out[code_pos] = code;
    out[wr++]     = 0x00; /* framing delimiter */
    return wr;
}

static size_t encode_slip(const unsigned char *in, size_t n, unsigned char *out, size_t cap) {
    size_t wr = 0;
    if (wr < cap) out[wr++] = SLIP_END;
    for (size_t i = 0; i < n; i++) {
        unsigned char b = in[i];
        if (b == SLIP_END) {
            if (wr + 2 > cap) return 0;
            out[wr++] = SLIP_ESC;
            out[wr++] = SLIP_ESC_END;
        } else if (b == SLIP_ESC) {
            if (wr + 2 > cap) return 0;
            out[wr++] = SLIP_ESC;
            out[wr++] = SLIP_ESC_ESC;
        } else {
            if (wr >= cap) return 0;
            out[wr++] = b;
        }
    }
    if (wr < cap) out[wr++] = SLIP_END;
    return wr;
}

static size_t encode_hdlc(const unsigned char *in, size_t n, unsigned char *out, size_t cap) {
    size_t wr = 0;
    if (wr < cap) out[wr++] = HDLC_FLAG;
    for (size_t i = 0; i < n; i++) {
        unsigned char b = in[i];
        if (b == HDLC_FLAG || b == HDLC_ESC) {
            if (wr + 2 > cap) return 0;
            out[wr++] = HDLC_ESC;
            out[wr++] = b ^ HDLC_ESCXOR;
        } else {
            if (wr >= cap) return 0;
            out[wr++] = b;
        }
    }
    if (wr < cap) out[wr++] = HDLC_FLAG;
    return wr;
}

static size_t encode_len16(const unsigned char *in, size_t n, unsigned char *out, size_t cap) {
    if (n > 0xFFFF || cap < n + 2) return 0;
    out[0] = (unsigned char)(n & 0xFF);
    out[1] = (unsigned char)((n >> 8) & 0xFF);
    memcpy(out + 2, in, n);
    return n + 2;
}

int framing_send(zt_ctx *c, const unsigned char *payload, size_t n) {
    if (!c || !payload) return -1;
    /* optionally append CRC into a temp buffer first */
    unsigned char        with_crc[ZT_LINEBUF_CAP + 4];
    const unsigned char *src  = payload;
    size_t               srcn = n;
    if (c->proto.crc_append && crc_size(c->proto.crc_mode) > 0) {
        size_t csz = crc_size(c->proto.crc_mode);
        if (n + csz > sizeof with_crc) return -1;
        memcpy(with_crc, payload, n);
        uint32_t crc = crc_compute(c->proto.crc_mode, payload, n);
        if (csz == 2) {
            with_crc[n]     = (unsigned char)((crc >> 8) & 0xFF);
            with_crc[n + 1] = (unsigned char)(crc & 0xFF);
        } else {
            with_crc[n]     = (unsigned char)((crc >> 24) & 0xFF);
            with_crc[n + 1] = (unsigned char)((crc >> 16) & 0xFF);
            with_crc[n + 2] = (unsigned char)((crc >> 8) & 0xFF);
            with_crc[n + 3] = (unsigned char)(crc & 0xFF);
        }
        src  = with_crc;
        srcn = n + csz;
    }

    unsigned char encoded[ZT_LINEBUF_CAP * 2 + 8];
    size_t        en = 0;
    switch (c->proto.mode) {
    case ZT_FRAME_COBS: en = encode_cobs(src, srcn, encoded, sizeof encoded); break;
    case ZT_FRAME_SLIP: en = encode_slip(src, srcn, encoded, sizeof encoded); break;
    case ZT_FRAME_HDLC: en = encode_hdlc(src, srcn, encoded, sizeof encoded); break;
    case ZT_FRAME_LENPFX: en = encode_len16(src, srcn, encoded, sizeof encoded); break;
    default:
        en = srcn;
        if (en > sizeof(encoded)) return -1;
        memcpy(encoded, src, srcn);
        break;
    }
    if (en == 0) return -1;
    direct_send(c, encoded, en);
    return (int)en;
}
