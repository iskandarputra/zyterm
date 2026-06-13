/**
 * @file test_tab_echo.c
 * @brief Tab-completion echo capture is OFF by default, ON only with --trusted.
 *
 * zyterm used to always mirror a device's Tab-completion echo into its own input
 * buffer. On a chatty device the asynchronous log stream interleaves with (or
 * stands in for) any echo, so that capture injected log fragments into the
 * command line — e.g. `skycab ... esp_` became
 * `skycab ... es RN_MSG : STATUS CHECK : SKYCAR NOT ALIGNED`. The default now is
 * the safe reconciliation model (ADR-0010): render_rx never touches the input
 * line. The fast legacy capture survives only behind the opt-in `--trusted` flag
 * (proto.trusted + proto.tab_echo), for a device the operator trusts.
 *
 * These tests pin both halves: default = input untouched; --trusted = the
 * completion echo is captured, the just-sent prefix is skipped (not duplicated),
 * and capture stops at the first log-output marker so async logs never leak in.
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

static zt_ctx *mkctx(void) {
    zt_ctx *c = calloc(1, sizeof *c);
    c->serial.fd    = -1;
    c->log.fd       = -1;
    c->net.http_fd  = -1;
    c->log.sb_lines = calloc(ZT_SCROLLBACK_CAP, sizeof(char *));
    return c;
}
static void freectx(zt_ctx *c) {
    scrollback_free(c);
    free(c->log.sb_lines);
    free(c);
}

static void test_input_untouched_by_rx(void) {
    SECTION("default mode: device RX never edits the local input line");
    static const char typed[] = "skycab eeprom read esp_";
    const size_t      n        = sizeof(typed) - 1;

    zt_ctx *c = mkctx(); /* trusted=false, tab_echo=false → capture disabled */
    memcpy(c->tui.input_buf, typed, n);
    c->tui.input_len = n;
    c->tui.sent_len  = n;

    /* Everything a chatty device might emit right after a Tab: a completion
     * suffix, redraw backspaces, cursor-forwards, a multi-candidate listing,
     * and an async <err> log line. None of it may reach the input buffer. */
    const char *barrage =
        "config\b\b\x1b[6Cesp_tc\x1b[2Cesp_config\r\n"
        "[00022960] <err> TC_SEQ: [-1] WRN_MSG : STATUS CHECK : SKYCAR NOT ALIGNED\r\n";
    render_rx(c, (const unsigned char *)barrage, strlen(barrage));

    ASSERT(c->tui.input_len == n, "input_len unchanged by device RX");
    ASSERT(memcmp(c->tui.input_buf, typed, n) == 0, "input text is exactly what was typed");

    freectx(c);
}

static void test_trusted_capture(void) {
    SECTION("--trusted: completion echo is captured, async log after CR is not");
    static const char typed[] = "skycab ee";
    const size_t      n        = sizeof(typed) - 1; /* 9 */

    zt_ctx *c       = mkctx();
    c->proto.trusted = true;
    memcpy(c->tui.input_buf, typed, n);
    c->tui.input_len = n;
    c->tui.sent_len  = n;

    /* Tab pressed with nothing unsent → skip=0; device echoes the completion
     * suffix, then CR/LF and an async log line that must NOT be captured. */
    c->proto.tab_echo = true;
    c->proto.tab_skip = 0;
    const char *echo = "prom\r\n[00022960] <err> TC_SEQ: STATUS CHECK\r\n";
    render_rx(c, (const unsigned char *)echo, strlen(echo));

    ASSERT(c->tui.input_len == n + 4 && memcmp(c->tui.input_buf, "skycab eeprom", n + 4) == 0,
           "completion suffix 'prom' appended → 'skycab eeprom'");
    ASSERT(!c->proto.tab_echo, "capture disarmed at the CR before the log line");

    freectx(c);
}

static void test_trusted_skip(void) {
    SECTION("--trusted: just-sent prefix echo is skipped, never duplicated");
    static const char typed[] = "ab";
    const size_t      n        = sizeof(typed) - 1; /* 2 */

    zt_ctx *c       = mkctx();
    c->proto.trusted = true;
    memcpy(c->tui.input_buf, typed, n);
    c->tui.input_len = n;
    c->tui.sent_len  = 1; /* only "a" had been sent; Tab flushed "b" */

    /* skip = unsent = input_len - sent_len = 1: the device first echoes the
     * just-flushed "b" (skip it) then the completion "cde" (append it). */
    c->proto.tab_echo = true;
    c->proto.tab_skip = 1;
    const char *echo = "bcde\r\n";
    render_rx(c, (const unsigned char *)echo, strlen(echo));

    ASSERT(c->tui.input_len == 5 && memcmp(c->tui.input_buf, "abcde", 5) == 0,
           "echoed 'b' skipped, 'cde' appended → 'abcde'");

    freectx(c);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_input_untouched_by_rx();
    test_trusted_capture();
    test_trusted_skip();

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "%d passed, %d failed\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");
    return g_fail == 0 ? 0 : 1;
}
