/**
 * @file test_tab_echo.c
 * @brief Device RX never edits the local input line.
 *
 * zyterm used to mirror a device's Tab-completion echo into its own input
 * buffer. On a chatty device the asynchronous log stream interleaves with (or
 * stands in for) any echo, so that capture injected log fragments into the
 * command line — e.g. `skycab ... esp_` became
 * `skycab ... es RN_MSG : STATUS CHECK : SKYCAR NOT ALIGNED`. The capture was
 * removed: the input line is now exactly what the user typed, and render_rx
 * must never touch it.
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

static void test_input_untouched_by_rx(void) {
    SECTION("device RX never edits the local input line");
    static const char typed[] = "skycab eeprom read esp_";
    const size_t      n        = sizeof(typed) - 1;

    zt_ctx c;
    memset(&c, 0, sizeof c);
    c.serial.fd     = -1;
    c.log.fd        = -1;
    c.net.http_fd   = -1;
    c.log.sb_lines  = calloc(ZT_SCROLLBACK_CAP, sizeof(char *));
    memcpy(c.tui.input_buf, typed, n);
    c.tui.input_len = n;
    c.tui.sent_len  = n;

    /* Everything a chatty device might emit right after a Tab: a completion
     * suffix, redraw backspaces, cursor-forwards, a multi-candidate listing,
     * and an async <err> log line. None of it may reach the input buffer. */
    const char *barrage =
        "config\b\b\x1b[6Cesp_tc\x1b[2Cesp_config\r\n"
        "[00022960] <err> TC_SEQ: [-1] WRN_MSG : STATUS CHECK : SKYCAR NOT ALIGNED\r\n";
    render_rx(&c, (const unsigned char *)barrage, strlen(barrage));

    ASSERT(c.tui.input_len == n, "input_len unchanged by device RX");
    ASSERT(memcmp(c.tui.input_buf, typed, n) == 0, "input text is exactly what was typed");

    scrollback_free(&c);
    free(c.log.sb_lines);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_input_untouched_by_rx();

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "%d passed, %d failed\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");
    return g_fail == 0 ? 0 : 1;
}
