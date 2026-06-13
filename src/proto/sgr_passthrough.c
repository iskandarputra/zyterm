/**
 * @file sgr_passthrough.c
 * @brief Bounded SGR-only filter for untrusted device RX (ADR-0009).
 *
 * The operator's terminal is a trust sink: bytes written to it can move the
 * cursor, set the title, or drive OSC 52 clipboard writes. Device RX is
 * untrusted, so by default zyterm neutralizes device escapes (INVARIANTS §6).
 * This filter relaxes that for the ONE escape class that cannot drive the
 * terminal — SGR (Select Graphic Rendition: colour/weight/underline,
 * `CSI … m`) — while still denying OSC (clipboard/title), cursor/erase,
 * private DECSET, DCS and every other control sequence.
 *
 * @ref sgr_feed is a pure, caller-owned-state byte classifier, so it is
 * unit-testable without a @ref zt_ctx and its state survives read()-chunk
 * boundaries. @c render_rx drives it per device byte in SGR-filter mode and
 * acts on the returned disposition: allowed SGR is stored verbatim in the
 * line buffer (rendered in-position at flush), everything else is rendered
 * as inert `cat -v` caret notation by the caller.
 *
 * Hardening: only digits, ';' and ':' are accepted as CSI parameters, so a
 * private/intermediate marker (`< = > ?` or 0x20-0x2F) — even with an `m`
 * final, e.g. `CSI ? 1 m` — is rejected; those are not real SGR. The param
 * buffer is fixed (@ref ZT_SGR_PARAM_CAP) and overflow aborts to inert, so
 * the parser can neither overrun nor buffer unbounded hostile input.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

/* True iff every accumulated CSI byte is a legal SGR parameter (0-9 ; :).
 * Any private-parameter marker (< = > ?) or intermediate (0x20-0x2F) fails,
 * which is what rejects `CSI ? 1 m` and friends even though the final is m. */
static bool sgr_params_ok(const zt_sgr_parser *st) {
    for (size_t i = 0; i < st->len; i++) {
        unsigned char b = st->buf[i];
        if (!((b >= '0' && b <= '9') || b == ';' || b == ':')) return false;
    }
    return true;
}

/* Reconstruct "ESC [ <buf> [final]" into out[]; returns the byte count.
 * out must hold at least ZT_SGR_PARAM_CAP + 4 bytes. */
static size_t sgr_rebuild(const zt_sgr_parser *st, unsigned char final, unsigned char *out) {
    size_t k = 0;
    out[k++] = 0x1B;
    out[k++] = '[';
    for (size_t i = 0; i < st->len; i++)
        out[k++] = st->buf[i];
    if (final) out[k++] = final;
    return k;
}

zt_sgr_act sgr_feed(zt_sgr_parser *st, unsigned char b, unsigned char *out, size_t *outlen) {
    switch (st->state) {
    case ZT_SGR_NONE:
        if (b == 0x1B) { /* hold the ESC; the next byte decides */
            st->state = ZT_SGR_ESC;
            return ZT_SGR_ACT_HOLD;
        }
        /* Defensive: caller only routes ESC or mid-sequence bytes here. */
        *outlen = 0;
        return ZT_SGR_ACT_REPROCESS;

    case ZT_SGR_ESC:
        if (b == '[') { /* CSI introducer — begin accumulating params */
            st->state = ZT_SGR_CSI;
            st->len   = 0;
            return ZT_SGR_ACT_HOLD;
        }
        /* ESC + anything else (OSC ']', DCS 'P', charset, 'c' reset, …):
         * neutralize the held ESC and re-feed this byte through the normal
         * path, so the whole sequence degrades to inert readable text. */
        st->state = ZT_SGR_NONE;
        out[0]    = 0x1B;
        *outlen   = 1;
        return ZT_SGR_ACT_REPROCESS;

    case ZT_SGR_CSI:
        if (b >= 0x20 && b <= 0x3F) {          /* param / intermediate byte */
            if (st->len >= ZT_SGR_PARAM_CAP) { /* overflow → abort to inert */
                st->state = ZT_SGR_NONE;
                *outlen   = sgr_rebuild(st, 0, out);
                return ZT_SGR_ACT_REPROCESS;
            }
            st->buf[st->len++] = b;
            return ZT_SGR_ACT_HOLD;
        }
        if (b >= 0x40 && b <= 0x7E) { /* final byte */
            st->state = ZT_SGR_NONE;
            if (b == 'm' && sgr_params_ok(st)) { /* the only allowed sequence */
                *outlen = sgr_rebuild(st, 'm', out);
                return ZT_SGR_ACT_EMIT_SGR;
            }
            *outlen = sgr_rebuild(st, b, out); /* non-SGR CSI → inert */
            return ZT_SGR_ACT_INERT;
        }
        /* Illegal mid-sequence byte (C0/DEL/\r/\n/NUL/C1): abort to inert and
         * re-feed the offending byte (so a mid-CSI \n still flushes the line). */
        st->state = ZT_SGR_NONE;
        *outlen   = sgr_rebuild(st, 0, out);
        return ZT_SGR_ACT_REPROCESS;
    }

    *outlen = 0; /* unreachable */
    return ZT_SGR_ACT_HOLD;
}
