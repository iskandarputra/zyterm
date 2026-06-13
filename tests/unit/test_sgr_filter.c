/**
 * @file test_sgr_filter.c
 * @brief Unit tests for the bounded SGR-only device-RX filter (ADR-0009).
 *
 * Two layers:
 *   1. Pure sgr_feed() state-machine matrix — allow real SGR, neutralize
 *      everything else, survive split-across-chunks, abort malformed input
 *      without overrun, and (the key security guard) reject `CSI ? 1 m`.
 *   2. One end-to-end pass through render_rx() with ob_set_record_callback()
 *      to confirm the integration wires the parser, line buffer, and bleed
 *      containment together.
 */
#define _GNU_SOURCE 1
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../src/zt_ctx.h"
#include "../../src/zt_internal.h"

static int g_pass, g_fail;

#define ASSERT(cond, msg)                                                                          \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
            fprintf(stderr, "  ok    %s\n", (msg));                                                \
        } else {                                                                                   \
            g_fail++;                                                                              \
            fprintf(stderr, "  FAIL  %s (%s:%d)\n", (msg), __FILE__, __LINE__);                    \
        }                                                                                          \
    } while (0)

#define SECTION(name) fprintf(stderr, "\n=== %s ===\n", (name))

/* cat -v caret form for one inert byte (mirrors render.c:emit_inert_byte). */
static size_t put_inert(unsigned char *o, unsigned char b) {
    if (b == 0x1B || b == 0x7F || (b < 0x20 && b != '\t')) {
        o[0] = '^';
        o[1] = (unsigned char)(b == 0x7F ? '?' : b + 0x40);
        return 2;
    }
    o[0] = b;
    return 1;
}

/* Drive bytes through sgr_feed exactly as render_rx's SGR-filter mode does,
 * producing the rendered byte stream: allowed SGR verbatim, all else inert.
 * State is caller-owned so a caller can split one logical stream across calls. */
static size_t filter_str(zt_sgr_parser *st, const char *in, size_t n, unsigned char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char)in[i];
    reprocess:
        if (st->state != ZT_SGR_NONE || b == 0x1B) {
            unsigned char seq[ZT_SGR_PARAM_CAP + 4];
            size_t        sl = 0;
            switch (sgr_feed(st, b, seq, &sl)) {
            case ZT_SGR_ACT_HOLD: goto next;
            case ZT_SGR_ACT_EMIT_SGR:
                for (size_t k = 0; k < sl; k++) out[o++] = seq[k];
                goto next;
            case ZT_SGR_ACT_INERT:
                for (size_t k = 0; k < sl; k++) o += put_inert(out + o, seq[k]);
                goto next;
            case ZT_SGR_ACT_REPROCESS:
                for (size_t k = 0; k < sl; k++) o += put_inert(out + o, seq[k]);
                goto reprocess;
            }
        }
        if (b == '\r') goto next;
        if (b == '\n') {
            out[o++] = '\n';
            goto next;
        }
        o += put_inert(out + o, b);
    next:;
    }
    out[o] = '\0';
    return o;
}

/* Convenience: one-shot filter from a fresh parser. */
static size_t filt(const char *in, unsigned char *out) {
    zt_sgr_parser st = {0};
    return filter_str(&st, in, strlen(in), out);
}

static bool has_esc(const unsigned char *buf, size_t n) {
    return memchr(buf, 0x1B, n) != NULL;
}

static void test_sgr_allow(void) {
    SECTION("sgr_feed — allowed SGR passes verbatim");
    unsigned char o[256];
    size_t        n;

    n = filt("\x1b[1;33m<wrn> hi\x1b[0m", o);
    ASSERT(memmem(o, n, "\x1b[1;33m", 7) != NULL, "bold-yellow SGR verbatim");
    ASSERT(memmem(o, n, "\x1b[0m", 4) != NULL, "reset SGR verbatim");
    ASSERT(memmem(o, n, "<wrn> hi", 8) != NULL, "text preserved");

    n = filt("\x1b[38;2;0;128;255mx", o);
    ASSERT(memmem(o, n, "\x1b[38;2;0;128;255m", 17) != NULL, "truecolor SGR verbatim");

    n = filt("\x1b[m", o); /* empty == reset */
    ASSERT(n == 3 && memcmp(o, "\x1b[m", 3) == 0, "empty SGR (ESC[m) verbatim");

    n = filt("\x1b[48;5;201;1;4m", o); /* 256-colour bg + attrs */
    ASSERT(memmem(o, n, "\x1b[48;5;201;1;4m", 15) != NULL, "256-colour SGR verbatim");
}

static void test_sgr_neutralize(void) {
    SECTION("sgr_feed — dangerous escapes neutralized (no raw ESC out)");
    unsigned char o[512];
    size_t        n;

    n = filt("\x1b]52;c;SGVsbG8=\x07x", o); /* OSC 52 clipboard */
    ASSERT(!has_esc(o, n), "OSC 52 leaves no raw ESC");
    ASSERT(memmem(o, n, "^[]52;c;SGVsbG8=^Gx", 19) != NULL, "OSC 52 rendered inert");

    n = filt("\x1b[2J", o); /* erase display */
    ASSERT(!has_esc(o, n) && memcmp(o, "^[[2J", 5) == 0, "CSI erase -> ^[[2J");

    n = filt("ab\x1b[3Ccd", o); /* CUF column padding in completion listings */
    ASSERT(!has_esc(o, n) && n == 7 && memcmp(o, "ab   cd", 7) == 0,
           "CUF (ESC[3C) renders as 3 spaces, not ^[[3C litter");

    n = filt("\x1b[?25h", o); /* DECSET hide cursor */
    ASSERT(!has_esc(o, n) && memcmp(o, "^[[?25h", 7) == 0, "DECSET -> inert");

    n = filt("\x1b[>0c", o); /* device attributes */
    ASSERT(!has_esc(o, n), "device-attr query inert");

    n = filt("\x1b]0;pwned\x07", o); /* OSC title set */
    ASSERT(!has_esc(o, n), "OSC title leaves no raw ESC");

    /* THE guard: private-parameter sequence with an `m` final is NOT SGR. */
    n = filt("\x1b[?1m", o);
    ASSERT(!has_esc(o, n) && memcmp(o, "^[[?1m", 6) == 0, "CSI ? 1 m rejected (not SGR)");

    n = filt("\x1b[1 m", o); /* intermediate 0x20 before m */
    ASSERT(!has_esc(o, n), "CSI with intermediate before m rejected");
}

static void test_sgr_split(void) {
    SECTION("sgr_feed — sequence split across chunks");
    unsigned char o[128];
    size_t        n1, n2;
    zt_sgr_parser st = {0};

    n1 = filter_str(&st, "\x1b[1;3", 5, o);          /* chunk 1: nothing emitted yet */
    ASSERT(n1 == 0, "partial SGR holds (no output mid-sequence)");
    n2 = filter_str(&st, "3mZ", 3, o);               /* chunk 2 completes it */
    ASSERT(memmem(o, n2, "\x1b[1;33m", 7) != NULL, "split SGR joins to ESC[1;33m");
    ASSERT(o[n2 - 1] == 'Z', "trailing text after split SGR");
}

static void test_sgr_malformed(void) {
    SECTION("sgr_feed — malformed / abort cases stay safe");
    unsigned char o[512];
    size_t        n;

    n = filt("\x1b[1\r2m", o); /* CR mid-CSI aborts */
    ASSERT(!has_esc(o, n), "CR mid-CSI aborts, no raw ESC");

    n = filt("\x1b[4\nx", o); /* LF mid-CSI aborts and flushes */
    ASSERT(!has_esc(o, n) && memchr(o, '\n', n) != NULL, "LF mid-CSI aborts + flushes");

    n = filt("\x1b\x1b[0m", o); /* ESC ESC then SGR */
    ASSERT(memmem(o, n, "\x1b[0m", 4) != NULL, "ESC ESC: second SGR honored");
    ASSERT(o[0] == '^' && o[1] == '[', "ESC ESC: first ESC neutralized");

    /* Overflow: ESC[ + 200 digits + m — must stay bounded, emit no SGR. */
    char big[210];
    big[0] = 0x1b;
    big[1] = '[';
    for (int i = 0; i < 200; i++) big[2 + i] = '1';
    big[202] = 'm';
    zt_sgr_parser st = {0};
    n            = filter_str(&st, big, 203, o);
    ASSERT(!has_esc(o, n), "200-digit overflow emits no verbatim SGR");
    ASSERT(st.len <= ZT_SGR_PARAM_CAP, "param buffer never overruns cap");
}

/* ---- end-to-end through render_rx + ob_set_record_callback ---- */

static unsigned char g_rec[8192];
static size_t        g_rec_len;
static void          rec_cb(const unsigned char *b, size_t n) {
    if (g_rec_len + n <= sizeof g_rec) {
        memcpy(g_rec + g_rec_len, b, n);
        g_rec_len += n;
    }
}

static void test_render_rx_e2e(void) {
    SECTION("render_rx — SGR filter end-to-end (default mode)");
    zt_ctx c;
    memset(&c, 0, sizeof c);
    c.serial.fd      = -1;
    c.log.fd         = -1;
    c.net.http_fd    = -1;
    c.proto.color_on = false;           /* isolate device SGR from zyterm's own */
    c.proto.sgr_passthrough = true;     /* the new default */
    c.log.sb_lines   = calloc(ZT_SCROLLBACK_CAP, sizeof(char *));

    g_rec_len = 0;
    ob_set_record_callback(rec_cb);
    const char *in = "\x1b[1;33mhi\x1b]52;c;QQ\x07\n";
    render_rx(&c, (const unsigned char *)in, strlen(in));
    ob_flush();
    ob_set_record_callback(NULL);

    ASSERT(memmem(g_rec, g_rec_len, "\x1b[1;33m", 7) != NULL, "device SGR reaches terminal");
    ASSERT(memmem(g_rec, g_rec_len, "\x1b]", 2) == NULL, "no raw OSC introducer reaches terminal");
    ASSERT(memmem(g_rec, g_rec_len, "^[]52;c;QQ^G", 12) != NULL, "OSC neutralized to caret text");
    ASSERT(memmem(g_rec, g_rec_len, "\x1b[0m", 4) != NULL, "bleed-containment reset emitted");

    scrollback_free(&c);
    free(c.log.sb_lines);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_sgr_allow();
    test_sgr_neutralize();
    test_sgr_split();
    test_sgr_malformed();
    test_render_rx_e2e();

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "%d passed, %d failed\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");
    return g_fail == 0 ? 0 : 1;
}
