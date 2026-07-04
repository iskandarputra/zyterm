/**
 * @file    http_sha1.c
 * @brief   SHA-1 + base64 primitives for the HTTP bridge.
 *
 * Just enough crypto to speak RFC 6455: SHA-1 (public domain, Steve Reid) and
 * a small base64 encoder. base64 also encodes RX bytes for the SSE/WS egress
 * (http_stream.c); the WebSocket accept-key (@c zt_ws_accept_key) is used by
 * the upgrade handshake in http_routes.c. Keeping the crypto here lets the rest
 * of the bridge stay plain socket code. No external dependency (single-binary
 * ethos). SHA-1 is used only for the WS handshake magic, never for security.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "http_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- SHA-1 (public domain, steve reid) ---- */
/* ---- SHA-1 (public domain, steve reid) ---- */
typedef struct {
    uint32_t      state[5];
    uint64_t      count;
    unsigned char buf[64];
} SHA1_CTX;
#define ROL(v, b) (((v) << (b)) | ((v) >> (32 - (b))))
static void sha1_tr(uint32_t st[5], const unsigned char b[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16) |
               ((uint32_t)b[i * 4 + 2] << 8) | b[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = st[0], bb = st[1], c = st[2], d = st[3], e = st[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (bb & c) | ((~bb) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = bb ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (bb & c) | (bb & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = bb ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e          = d;
        d          = c;
        c          = ROL(bb, 30);
        bb         = a;
        a          = t;
    }
    st[0] += a;
    st[1] += bb;
    st[2] += c;
    st[3] += d;
    st[4] += e;
}
static void sha1_init(SHA1_CTX *c) {
    c->state[0] = 0x67452301;
    c->state[1] = 0xEFCDAB89;
    c->state[2] = 0x98BADCFE;
    c->state[3] = 0x10325476;
    c->state[4] = 0xC3D2E1F0;
    c->count    = 0;
}
static void sha1_update(SHA1_CTX *c, const unsigned char *d, size_t n) {
    size_t i = (c->count >> 3) & 63;
    c->count += (uint64_t)n << 3;
    size_t j = 64 - i;
    if (n < j) {
        memcpy(c->buf + i, d, n);
        return;
    }
    memcpy(c->buf + i, d, j);
    sha1_tr(c->state, c->buf);
    size_t k = j;
    while (k + 64 <= n) {
        sha1_tr(c->state, d + k);
        k += 64;
    }
    memcpy(c->buf, d + k, n - k);
}
static void sha1_final(SHA1_CTX *c, unsigned char out[20]) {
    uint64_t      bits = c->count;
    unsigned char pad  = 0x80;
    sha1_update(c, &pad, 1);
    unsigned char zero = 0;
    while (((c->count >> 3) & 63) != 56)
        sha1_update(c, &zero, 1);
    unsigned char blen[8];
    for (int i = 0; i < 8; i++)
        blen[i] = (unsigned char)(bits >> (56 - i * 8));
    sha1_update(c, blen, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4 + 0] = (unsigned char)(c->state[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->state[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->state[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(c->state[i]);
    }
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
void              zt_base64(const unsigned char *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned)in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? B64[v & 63] : '=';
    }
    out[o] = '\0';
}

/* Compute the RFC 6455 Sec-WebSocket-Accept for a client key: base64(SHA-1(key
 * || magic-GUID)). @p out needs >= 32 bytes (28-char accept + NUL). */
void zt_ws_accept_key(const char *key, char *out, size_t cap) {
    static const char kGUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char              concat[256];
    int               cn = snprintf(concat, sizeof concat, "%s%s", key, kGUID);
    SHA1_CTX          ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, (const unsigned char *)concat, snprintf_len(cn, sizeof concat));
    unsigned char digest[20];
    sha1_final(&ctx, digest);
    char accept[64];
    zt_base64(digest, 20, accept);
    snprintf(out, cap, "%s", accept);
}
