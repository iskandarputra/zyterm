/**
 * @file line_endings.c
 * @brief End-of-line translation between host and device.
 *
 * Implements the matrix of CR / LF / CRLF rewrites selected by the
 * `--map-out` and `--map-in` CLI flags. See @ref zt_eol_map in
 * @ref zt_ctx.h for the semantics of each mode.
 *
 * Both directions are streaming — translators may be called with bytes
 * arriving in arbitrary chunks (e.g. CR at the end of one read(),
 * LF at the start of the next). A one-byte @ref zt_eol_state latch
 * carries the "saw CR" flag across calls so CRLF coalescing is correct
 * at chunk boundaries.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"
#include <string.h>

/* ── helpers ───────────────────────────────────────────────────────────── */

#define EMIT(b)                                                                \
    do {                                                                       \
        if (w >= out_cap) return w;                                            \
        out[w++] = (unsigned char) (b);                                        \
    } while (0)

const char *eol_name(zt_eol_map m) {
    switch (m) {
    case ZT_EOL_NONE:    return "none";
    case ZT_EOL_CR:      return "cr";
    case ZT_EOL_LF:      return "lf";
    case ZT_EOL_CRLF:    return "crlf";
    case ZT_EOL_CR_CRLF: return "cr-crlf";
    case ZT_EOL_LF_CRLF: return "lf-crlf";
    default:             return "?";
    }
}

int eol_parse(const char *token, zt_eol_map *out) {
    if (!token || !out) return -1;
    if (!strcmp(token, "none"))    { *out = ZT_EOL_NONE;    return 0; }
    if (!strcmp(token, "cr"))      { *out = ZT_EOL_CR;      return 0; }
    if (!strcmp(token, "lf"))      { *out = ZT_EOL_LF;      return 0; }
    if (!strcmp(token, "crlf"))    { *out = ZT_EOL_CRLF;    return 0; }
    if (!strcmp(token, "cr-crlf")) { *out = ZT_EOL_CR_CRLF; return 0; }
    if (!strcmp(token, "lf-crlf")) { *out = ZT_EOL_LF_CRLF; return 0; }
    return -1;
}

/* ── outgoing (host → device) ─────────────────────────────────────────── */

size_t eol_translate_out(zt_eol_map mode, zt_eol_state *st,
                         const unsigned char *in, size_t n,
                         unsigned char *out, size_t out_cap) {
    if (mode == ZT_EOL_NONE || !in || !out) {
        size_t k = (n < out_cap) ? n : out_cap;
        if (k && in && out) memcpy(out, in, k);
        return k;
    }

    size_t w = 0;
    /* Outgoing path is single-byte stateless except for ZT_EOL_CRLF, which
     * needs to know "did the previous outgoing byte come from a CR" so a
     * CR LF sequence in the user's typed buffer is collapsed into one
     * CRLF rather than CRLFLF / CRLFCRLF. */
    uint8_t saw_cr = st ? st->saw_cr : 0u;

    for (size_t i = 0; i < n; i++) {
        unsigned char b = in[i];

        switch (mode) {
        case ZT_EOL_CR:
            /* LF → CR; everything else passthrough. */
            EMIT(b == '\n' ? '\r' : b);
            break;

        case ZT_EOL_LF:
            /* CR → LF; everything else passthrough. */
            EMIT(b == '\r' ? '\n' : b);
            break;

        case ZT_EOL_CRLF:
            /* LF → CRLF; lone CR → CRLF; CRLF stays CRLF. */
            if (b == '\r') {
                EMIT('\r'); EMIT('\n');
                saw_cr = 1;
            } else if (b == '\n') {
                if (saw_cr) {
                    /* This LF was the second half of a user-typed CRLF;
                     * we already emitted CRLF for the CR. Skip. */
                } else {
                    EMIT('\r'); EMIT('\n');
                }
                saw_cr = 0;
            } else {
                EMIT(b);
                saw_cr = 0;
            }
            break;

        case ZT_EOL_CR_CRLF:
            /* CR → CRLF; LF passthrough. */
            if (b == '\r') { EMIT('\r'); EMIT('\n'); }
            else           { EMIT(b); }
            break;

        case ZT_EOL_LF_CRLF:
            /* LF → CRLF; CR passthrough. */
            if (b == '\n') { EMIT('\r'); EMIT('\n'); }
            else           { EMIT(b); }
            break;

        default:
            EMIT(b);
            break;
        }
    }

    if (st) st->saw_cr = saw_cr;
    return w;
}

/* ── incoming (device → host) ─────────────────────────────────────────── */

size_t eol_translate_in(zt_eol_map mode, zt_eol_state *st,
                        const unsigned char *in, size_t n,
                        unsigned char *out, size_t out_cap) {
    if (mode == ZT_EOL_NONE || !in || !out) {
        size_t k = (n < out_cap) ? n : out_cap;
        if (k && in && out) memcpy(out, in, k);
        return k;
    }

    size_t  w      = 0;
    uint8_t saw_cr = st ? st->saw_cr : 0u;

    for (size_t i = 0; i < n; i++) {
        unsigned char b = in[i];

        switch (mode) {
        case ZT_EOL_CR:
            /* CR → LF; passthrough otherwise. (Useful for old Mac firmware.) */
            EMIT(b == '\r' ? '\n' : b);
            break;

        case ZT_EOL_LF:
            /* LF → CR; passthrough otherwise. */
            EMIT(b == '\n' ? '\r' : b);
            break;

        case ZT_EOL_CRLF:
        case ZT_EOL_LF_CRLF:
            /* CRLF → LF. Lone CR or lone LF passthrough as-is. */
            if (saw_cr) {
                if (b == '\n')      { EMIT('\n'); saw_cr = 0; }
                else if (b == '\r') { EMIT('\r'); /* keep saw_cr = 1 */ }
                else                { EMIT('\r'); EMIT(b); saw_cr = 0; }
            } else if (b == '\r') {
                saw_cr = 1;
            } else {
                EMIT(b);
            }
            break;

        case ZT_EOL_CR_CRLF:
            /* CRLF → CR; lone CR / LF passthrough. */
            if (saw_cr) {
                if (b == '\n')      { EMIT('\r'); saw_cr = 0; }
                else if (b == '\r') { EMIT('\r'); /* keep saw_cr = 1 */ }
                else                { EMIT('\r'); EMIT(b); saw_cr = 0; }
            } else if (b == '\r') {
                saw_cr = 1;
            } else {
                EMIT(b);
            }
            break;

        default:
            EMIT(b);
            break;
        }
    }

    if (st) st->saw_cr = saw_cr;
    return w;
}
