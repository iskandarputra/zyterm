/**
 * @file test_logio.c
 * @brief Behavioural test for log_write() line-span batching (logio.c).
 *
 * log_write now emits one write(2) per line-span instead of per byte (a
 * high-baud syscall-storm fix). This guards that the *bytes* written are
 * unchanged: a timestamp at each line start, the payload verbatim, and the
 * line_start state carried correctly across calls. @author Iskandar Putra
 * @license MIT
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

/* Strip each "[...] " timestamp prefix log_emit_ts writes; return the payload
 * that remains and the count of stripped timestamps. (The test payloads never
 * contain '[', so this is unambiguous.) */
static size_t strip_ts(const char *in, size_t n, char *out, int *nts) {
    size_t o = 0;
    *nts     = 0;
    for (size_t i = 0; i < n;) {
        if (in[i] == '[') {
            const char *close = memmem(in + i, n - i, "] ", 2);
            if (close) {
                (*nts)++;
                i = (size_t)(close - in) + 2;
                continue;
            }
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
    return o;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    SECTION("log_write — line-span batching preserves bytes + line-start state");
    char tmpl[] = "/tmp/zyterm_logio_XXXXXX";
    int  fd     = mkstemp(tmpl);
    ASSERT(fd >= 0, "temp log created");
    if (fd < 0) return 1;

    zt_ctx c;
    memset(&c, 0, sizeof c);
    c.log.fd         = fd;
    c.log.line_start = true;

    /* Three lines (last has no trailing newline) in one call. */
    log_write(&c, (const unsigned char *)"A\nB\nC", 5);
    /* Continuation call: line_start is false after "C", so "D\n" appends to the
     * C line with NO new timestamp, then the next line would start fresh. */
    log_write(&c, (const unsigned char *)"D\n", 2);

    /* read the file back */
    lseek(fd, 0, SEEK_SET);
    char    raw[4096];
    ssize_t rn = read(fd, raw, sizeof raw - 1);
    ASSERT(rn > 0, "log file has content");
    raw[rn > 0 ? rn : 0] = '\0';

    char payload[4096];
    int  nts = 0;
    strip_ts(raw, (size_t)(rn > 0 ? rn : 0), payload, &nts);

    ASSERT(strcmp(payload, "A\nB\nCD\n") == 0, "payload bytes verbatim, newlines intact");
    ASSERT(nts == 3, "one timestamp per line start (A, B, C); none before the D continuation");

    close(fd);
    unlink(tmpl);

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "%d passed, %d failed\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");
    return g_fail == 0 ? 0 : 1;
}
