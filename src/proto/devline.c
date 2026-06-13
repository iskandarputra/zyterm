/**
 * @file devline.c
 * @brief Device prompt-line model + Tab-completion reconciliation (ADR-0010).
 *
 * The user types into zyterm's local input line; on Tab, zyterm forwards the
 * keystroke and the device completes. We want that completion mirrored into the
 * local input line — robustly, even though the device (a Zephyr shell) emits
 * continuous asynchronous log output.
 *
 * The trick: model only the device's CURRENT line. @ref devline_feed rebuilds
 * the visible characters of that line by interpreting a minimal line discipline
 * (printable, CR, BS, LF, and the `ESC[K` / `ESC[nC` / `ESC[nD` redraw escapes),
 * and RESETS on '\n'. Async log lines are newline-terminated and the device
 * reprints its prompt afterwards, so they never pollute the current-line model.
 *
 * Reconciliation (@ref devline_ingest): inside the post-Tab window, find the
 * exact command the user sent within the device line and adopt whatever the
 * device appended after it — the completion. The adoption is APPEND-ONLY
 * (never alters typed bytes), content-whitelisted, and length-capped. The
 * core functions are pure (caller-owned state, no zt_ctx) and unit-testable.
 *
 * Trust (INVARIANTS §6): Enter sends only `\r` and the device already holds the
 * completed line, so reconciliation never makes zyterm originate bytes to the
 * device — the adopted tail is display + history only. See ADR-0010.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <string.h>

void devline_reset(zt_devline *st) {
    if (!st) return;
    memset(st, 0, sizeof *st);
}

/* Parse the leading integer of the accumulated CSI params; default 1, as in
 * ECMA-48 (an omitted/zero parameter for CUF/CUB means 1). */
static size_t csi_num(const zt_devline *st) {
    size_t v = 0;
    for (size_t i = 0; i < st->csi_len; i++) {
        unsigned char b = st->csi[i];
        if (b < '0' || b > '9') break; /* stop at ';' or an intermediate */
        v = v * 10 + (size_t)(b - '0');
        if (v > ZT_DEVLINE_CAP) v = ZT_DEVLINE_CAP; /* clamp */
    }
    return v ? v : 1;
}

/* Write one printable/UTF-8 byte at the cursor, advancing it. Fills any gap
 * left by a cursor-forward with spaces so the buffer stays contiguous. */
static void dl_putc(zt_devline *st, unsigned char b) {
    if (st->col >= ZT_DEVLINE_CAP) {
        st->overflowed = true;
        return;
    }
    while (st->len < st->col && st->len < ZT_DEVLINE_CAP)
        st->buf[st->len++] = ' ';
    st->buf[st->col++] = b;
    if (st->col > st->len) st->len = st->col;
}

void devline_feed(zt_devline *st, unsigned char b) {
    if (!st) return;

    switch (st->st) {
    case ZT_DL_ESC:
        if (b == '[') {
            st->st      = ZT_DL_CSI;
            st->csi_len = 0;
        } else if (b == ']') {
            st->st = ZT_DL_OSC; /* OSC — consume to BEL or ST */
        } else {
            st->st = ZT_DL_NONE; /* 2-byte escape (charset, RIS, …): consumed */
        }
        return;

    case ZT_DL_CSI:
        if (b >= 0x20 && b <= 0x3F) { /* params + intermediates */
            if (st->csi_len < ZT_DEVLINE_CSI_CAP) st->csi[st->csi_len++] = b;
            return;
        }
        if (b >= 0x40 && b <= 0x7E) { /* final byte */
            size_t n = csi_num(st);
            switch (b) {
            case 'C': /* CUF — cursor forward */
                st->col += n;
                if (st->col > ZT_DEVLINE_CAP) st->col = ZT_DEVLINE_CAP;
                break;
            case 'D': /* CUB — cursor back */ st->col = (n < st->col) ? st->col - n : 0; break;
            case 'K': /* EL — erase in line */
                if (st->csi_len == 0 || st->csi[0] == '0') {
                    st->len = st->col; /* erase to end */
                } else if (st->csi[0] == '1') {
                    for (size_t i = 0; i < st->col && i < st->len; i++)
                        st->buf[i] = ' ';
                } else if (st->csi[0] == '2') {
                    st->len = 0;
                    st->col = 0;
                }
                break;
            case 'J': /* ED — erase display (e.g. clear-screen): reset the line */
                st->len = 0;
                st->col = 0;
                break;
            default: break; /* other finals: consumed, ignored */
            }
            st->st = ZT_DL_NONE;
            return;
        }
        st->st = ZT_DL_NONE; /* malformed CSI (C0/ESC mid-sequence): abandon */
        return;

    case ZT_DL_OSC:
        if (b == 0x07)
            st->st = ZT_DL_NONE; /* BEL terminates */
        else if (b == 0x1B)
            st->st = ZT_DL_ESC; /* ST (ESC \) — re-enter ESC */
        return;                 /* otherwise consume OSC body */

    case ZT_DL_NONE:
    default: break;
    }

    switch (b) {
    case 0x1B: st->st = ZT_DL_ESC; return; /* ESC */
    case '\n':                             /* LF — line committed, reset */
        st->len = st->col = 0;
        st->overflowed    = false;
        return;
    case '\r': st->col = 0; return; /* CR — carriage return */
    case 0x08:                      /* BS — cursor left */
        if (st->col > 0) st->col--;
        return;
    case 0x7F: return; /* DEL — ignore */
    case '\t': return; /* TAB — not part of the line */
    default: break;
    }
    if (b < 0x20) return; /* other C0 controls: ignore */
    dl_putc(st, b);       /* printable / UTF-8 */
}

/* Minimal check that buf[0..n) is whole, control-free UTF-8 (or ASCII).
 * Rejects embedded control/DEL and a sequence truncated at the end (so an
 * in-flight multibyte char makes us wait for the next chunk). */
static bool tail_clean(const unsigned char *buf, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char b = buf[i];
        if (b < 0x20 || b == 0x7F) return false; /* control/DEL not allowed */
        size_t need;
        if (b < 0x80)
            need = 0;
        else if ((b & 0xE0) == 0xC0)
            need = 1;
        else if ((b & 0xF0) == 0xE0)
            need = 2;
        else if ((b & 0xF8) == 0xF0)
            need = 3;
        else
            return false;                   /* stray continuation / invalid lead */
        if (i + 1 + need > n) return false; /* truncated multibyte at end */
        for (size_t k = 0; k < need; k++)
            if ((buf[i + 1 + k] & 0xC0) != 0x80) return false;
        i += 1 + need;
    }
    return true;
}

bool devline_tail(const zt_devline *st, const unsigned char *cmd, size_t cmd_len,
                  const unsigned char **tail, size_t *tail_len) {
    if (!st || !cmd || cmd_len == 0 || st->overflowed) return false;
    if (cmd_len > st->len) return false;

    /* Last (rightmost) occurrence of cmd in the current device line — that is
     * the live prompt echo, not a coincidental match inside the prompt. */
    size_t best = (size_t)-1;
    for (size_t start = 0; start + cmd_len <= st->len; start++)
        if (memcmp(st->buf + start, cmd, cmd_len) == 0) best = start;
    if (best == (size_t)-1) return false;

    size_t end = best + cmd_len;
    size_t tl  = st->len - end; /* everything the device appended after cmd */
    if (tl == 0 || tl > ZT_RECONCILE_TAIL_MAX) return false;
    if (cmd_len + tl >= ZT_INPUT_CAP) return false;
    if (!tail_clean(st->buf + end, tl)) return false;

    *tail     = st->buf + end;
    *tail_len = tl;
    return true;
}

void devline_ingest(zt_ctx *c, const unsigned char *buf, size_t n) {
    if (!c || !buf) return;

    for (size_t i = 0; i < n; i++)
        devline_feed(&c->proto.devline, buf[i]);

    if (!c->tui.reconcile_pending) return;

    /* Expire the post-Tab window. */
    struct timespec t;
    now(&t);
    if (ts_diff_sec(&t, &c->tui.reconcile_armed) > ZT_RECONCILE_WINDOW_MS / 1000.0) {
        c->tui.reconcile_pending = false;
        return;
    }

    /* Anchor on the exact command the user sent (input_buf[0..reconcile_cmd_len)
     * — still the prefix of input_buf because adoption is append-only). Only
     * touch the line when there is no unsent edit in flight. */
    size_t cmd_len = c->tui.reconcile_cmd_len;
    if (cmd_len == 0 || cmd_len > c->tui.input_len) return;
    if (c->tui.sent_len != c->tui.input_len) return;

    const unsigned char *tail;
    size_t               tail_len;
    if (!devline_tail(&c->proto.devline, c->tui.input_buf, cmd_len, &tail, &tail_len)) return;

    /* Append-only adoption: rewrite only the region after the typed command. */
    memcpy(c->tui.input_buf + cmd_len, tail, tail_len);
    c->tui.input_len = c->tui.sent_len = cmd_len + tail_len;
    c->tui.cursor                      = 0;
    c->tui.ui_dirty                    = true;
}
