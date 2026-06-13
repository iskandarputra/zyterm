/**
 * @file test_devline.c
 * @brief Device prompt-line model + Tab-completion reconciliation (ADR-0010).
 *
 * Layer 1 — pure `devline_feed` line discipline (CR/BS/LF/ESC[K/ESC[nC/ESC[nD,
 * OSC consume, overflow). Layer 2 — pure `devline_tail` (anchor + whitelist +
 * caps). Layer 3 — `devline_ingest` reconciliation incl. the regression that
 * motivated this: an async log burst must NOT leak into the input line.
 */
#define _GNU_SOURCE 1
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void feed(zt_devline *st, const char *s) {
    for (const char *p = s; *p; p++) devline_feed(st, (unsigned char)*p);
}
static bool dl_eq(const zt_devline *st, const char *want) {
    size_t n = strlen(want);
    return st->len == n && memcmp(st->buf, want, n) == 0;
}

static void test_feed(void) {
    SECTION("devline_feed — line discipline");
    zt_devline d;

    devline_reset(&d);
    feed(&d, "SkyCar:~$ esp_");
    ASSERT(dl_eq(&d, "SkyCar:~$ esp_"), "plain build");

    devline_reset(&d);
    feed(&d, "foo\rbar");
    ASSERT(dl_eq(&d, "bar"), "CR returns to col 0, overwrites");

    devline_reset(&d);
    feed(&d, "config\b\b\bESP");
    ASSERT(dl_eq(&d, "conESP"), "backspace moves cursor; next chars overwrite");

    devline_reset(&d);
    feed(&d, "hello\x1b[3D\x1b[K");
    ASSERT(dl_eq(&d, "he"), "CUB 3 then EL erases to end");

    devline_reset(&d);
    feed(&d, "ab\x1b[3Ccd");
    ASSERT(dl_eq(&d, "ab   cd"), "CUF fills the gap with spaces");

    devline_reset(&d);
    feed(&d, "abc\ndef");
    ASSERT(dl_eq(&d, "def"), "LF resets the line (async-log immunity)");

    devline_reset(&d);
    feed(&d, "a\x1b]0;window-title\x07z");
    ASSERT(dl_eq(&d, "az"), "OSC body consumed to BEL");

    devline_reset(&d);
    feed(&d, "a\x1b[2Jz");
    ASSERT(dl_eq(&d, "z"), "ESC[2J clears the line");

    devline_reset(&d);
    for (int i = 0; i < ZT_DEVLINE_CAP + 16; i++) devline_feed(&d, 'x');
    ASSERT(d.overflowed && d.len <= ZT_DEVLINE_CAP, "overflow flag set, bounded");
}

static void test_tail(void) {
    SECTION("devline_tail — anchor + whitelist + caps");
    zt_devline          d;
    const unsigned char *t;
    size_t               tl;

    devline_reset(&d);
    feed(&d, "SkyCar:~$ esp_config");
    ASSERT(devline_tail(&d, (const unsigned char *)"esp_", 4, &t, &tl) && tl == 6 &&
               memcmp(t, "config", 6) == 0,
           "unique completion → tail 'config'");

    devline_reset(&d);
    feed(&d, "SkyCar:~$ esp_");
    ASSERT(!devline_tail(&d, (const unsigned char *)"esp_", 4, &t, &tl),
           "no completion (cmd at end) → no-adopt");

    devline_reset(&d);
    feed(&d, "esp_ in prompt... SkyCar:~$ esp_config");
    ASSERT(devline_tail(&d, (const unsigned char *)"esp_", 4, &t, &tl) && memcmp(t, "config", 6) == 0,
           "double occurrence → rightmost (live) anchor");

    devline_reset(&d);
    feed(&d, "SkyCar:~$ skycab [00512770] <err> BATTERY_MANAGER FAILED");
    ASSERT(devline_tail(&d, (const unsigned char *)"sky", 3, &t, &tl) && tl == 3 &&
               memcmp(t, "cab", 3) == 0,
           "inline log on prompt line: tail stops at space → 'cab'");

    devline_reset(&d);
    feed(&d, "SkyCar:~$ skycab ");
    ASSERT(devline_tail(&d, (const unsigned char *)"sky", 3, &t, &tl) && tl == 3 &&
               memcmp(t, "cab", 3) == 0,
           "trailing space ends the tail (single token)");

    devline_reset(&d);
    feed(&d, "SkyCar:~$ esp_[00020191] <err> X");
    ASSERT(!devline_tail(&d, (const unsigned char *)"esp_", 4, &t, &tl),
           "ambiguous + inline log ('esp_[...') → no-adopt");

    /* Crafted: a control byte in the tail must be rejected. */
    devline_reset(&d);
    memcpy(d.buf, "cmd\x07x", 5);
    d.len = 5;
    d.col = 5;
    ASSERT(!devline_tail(&d, (const unsigned char *)"cmd", 3, &t, &tl), "control byte in tail rejected");

    /* Crafted: tail longer than the cap. */
    devline_reset(&d);
    memcpy(d.buf, "cmd", 3);
    for (size_t i = 0; i < ZT_RECONCILE_TAIL_MAX + 5; i++) d.buf[3 + i] = 'y';
    d.len = 3 + ZT_RECONCILE_TAIL_MAX + 5;
    d.col = d.len;
    ASSERT(!devline_tail(&d, (const unsigned char *)"cmd", 3, &t, &tl), "over-long tail rejected");

    /* Crafted: truncated UTF-8 lead at end → wait (no-adopt). */
    devline_reset(&d);
    memcpy(d.buf, "cmd\xC3", 4);
    d.len = 4;
    d.col = 4;
    ASSERT(!devline_tail(&d, (const unsigned char *)"cmd", 3, &t, &tl), "truncated UTF-8 tail rejected");
}

/* ---- reconciliation through devline_ingest ---- */

static void arm(zt_ctx *c, const char *typed, bool in_window) {
    memset(c, 0, sizeof *c);
    c->serial.fd      = -1;
    c->log.fd         = -1;
    c->net.http_fd    = -1;
    c->proto.mode     = ZT_FRAME_RAW;
    c->log.sb_lines   = calloc(ZT_SCROLLBACK_CAP, sizeof(char *));
    size_t n          = strlen(typed);
    memcpy(c->tui.input_buf, typed, n);
    c->tui.input_len  = c->tui.sent_len = n;
    c->tui.cursor     = 0;
    c->tui.reconcile_pending = true;
    c->tui.reconcile_cmd_len = n;
    now(&c->tui.reconcile_armed);
    if (!in_window) c->tui.reconcile_armed.tv_sec -= 5; /* push past the window */
}
static bool in_eq(const zt_ctx *c, const char *want) {
    size_t n = strlen(want);
    return c->tui.input_len == n && memcmp(c->tui.input_buf, want, n) == 0;
}
static void feed_ctx(zt_ctx *c, const char *s) {
    devline_ingest(c, (const unsigned char *)s, strlen(s));
}

static void test_reconcile(void) {
    SECTION("devline_ingest — completion reconciliation");
    zt_ctx c;

    /* unique completion adopts */
    arm(&c, "esp_", true);
    feed_ctx(&c, "SkyCar:~$ esp_");  /* prior prompt echo */
    feed_ctx(&c, "config");          /* device appends the completion */
    ASSERT(in_eq(&c, "esp_config") && c.tui.sent_len == c.tui.input_len, "unique completion adopted");
    free(c.log.sb_lines);

    /* inline log on the prompt line (no newline) — the exact screenshot bug:
     * adopt only the completed token, never the appended log. */
    arm(&c, "sky", true);
    feed_ctx(&c, "SkyCar:~$ sky");
    feed_ctx(&c, "cab [00512770] <err> BATTERY_MANAGER: [BMGR][-1] SEND BMS REQUEST FAILED");
    ASSERT(in_eq(&c, "skycab"), "inline-log completion adopts only 'skycab'");
    free(c.log.sb_lines);

    /* the regression: an async log burst must NOT leak into the input line */
    arm(&c, "esp_", true);
    feed_ctx(&c, "SkyCar:~$ esp_");
    feed_ctx(&c, "\r\n[00022960] <err> TC_SEQ: [-1] WRN_MSG : STATUS CHECK : SKYCAR NOT ALIGNED\r\n");
    ASSERT(in_eq(&c, "esp_"), "log burst does NOT leak into input");
    feed_ctx(&c, "SkyCar:~$ esp_config"); /* device reprints prompt + completes */
    ASSERT(in_eq(&c, "esp_config"), "completion after the log reprint still adopts");
    free(c.log.sb_lines);

    /* ambiguous: device lists candidates then reprints prompt → no tail */
    arm(&c, "esp_", true);
    feed_ctx(&c, "SkyCar:~$ esp_");
    feed_ctx(&c, "\r\n  esp_tc  esp_config\r\nSkyCar:~$ esp_");
    ASSERT(in_eq(&c, "esp_"), "ambiguous completion leaves input unchanged");
    free(c.log.sb_lines);

    /* window expired → no adoption */
    arm(&c, "esp_", false);
    feed_ctx(&c, "SkyCar:~$ esp_config");
    ASSERT(in_eq(&c, "esp_") && !c.tui.reconcile_pending, "expired window adopts nothing");
    free(c.log.sb_lines);

    /* not armed (user already editing) → no adoption */
    arm(&c, "esp_", true);
    c.tui.reconcile_pending = false;
    feed_ctx(&c, "SkyCar:~$ esp_config");
    ASSERT(in_eq(&c, "esp_"), "unarmed → no adoption");
    free(c.log.sb_lines);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_feed();
    test_tail();
    test_reconcile();

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "%d passed, %d failed\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");
    return g_fail == 0 ? 0 : 1;
}
