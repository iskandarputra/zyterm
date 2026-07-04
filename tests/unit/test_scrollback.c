/**
 * @file test_scrollback.c
 * @brief Behavioural test for the scrollback ring (scrollback.c).
 *
 * scrollback_push() now recycles each slot's allocation instead of
 * malloc()/free()-ing one buffer per line — a pure allocator-churn win. This
 * guards that the observable ring semantics are unchanged: append order, FIFO
 * eviction + wrap once full, correct newest/oldest indexing, and that a slot
 * reused with a shorter line reads back cleanly (no leftover bytes past the
 * NUL). It also pins the two allocator behaviours the rewrite promises: a
 * short line reuses the slot's buffer in place (same pointer, no realloc), and
 * a longer line grows it. Run under ASan for the free path (scrollback_free).
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @license MIT — see LICENSE for details.
 */
#define _GNU_SOURCE 1
#include <malloc.h> /* malloc_usable_size */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Stage a line into the ring's line buffer and push it. */
static void push_str(zt_ctx *c, const char *s) {
    size_t n = strlen(s);
    if (n > sizeof c->log.line) n = sizeof c->log.line;
    memcpy(c->log.line, s, n);
    c->log.line_len = n;
    scrollback_push(c);
}

/* The reader indexing used by scrollback_view.c / search.c:
 * from_newest==0 is the most-recently pushed line. */
static const char *line_at(zt_ctx *c, int from_newest) {
    int idx = (c->log.sb_head + c->log.sb_count - 1 - from_newest) % ZT_SCROLLBACK_CAP;
    if (idx < 0) idx += ZT_SCROLLBACK_CAP;
    return c->log.sb_lines[idx];
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    zt_ctx c;
    memset(&c, 0, sizeof c);
    c.log.sb_lines = calloc(ZT_SCROLLBACK_CAP, sizeof(char *));
    if (!c.log.sb_lines) {
        fprintf(stderr, "  SKIP  out of memory allocating ring\n");
        return 0;
    }

    SECTION("append: order, count, newest/oldest");
    push_str(&c, "alpha");
    push_str(&c, "bravo");
    push_str(&c, "charlie");
    ASSERT(c.log.sb_count == 3, "count tracks appended lines");
    ASSERT(c.log.sb_head == 0, "head unmoved while filling");
    ASSERT(strcmp(line_at(&c, 0), "charlie") == 0, "newest is last pushed");
    ASSERT(strcmp(line_at(&c, 2), "alpha") == 0, "oldest is first pushed");

    SECTION("empty / zero-length lines are ignored");
    int before     = c.log.sb_count;
    c.log.line_len = 0;
    scrollback_push(&c);
    ASSERT(c.log.sb_count == before, "zero-length push is a no-op");

    SECTION("in-place reuse: capture slot 0 pointer");
    /* Reset the ring and seed slot 0 with a long line so it has spare capacity. */
    for (int i = 0; i < c.log.sb_count; i++) {
        free(c.log.sb_lines[i]);
        c.log.sb_lines[i] = NULL;
    }
    c.log.sb_count = c.log.sb_head = 0;
    char longline[128];
    memset(longline, 'x', sizeof longline - 1);
    longline[sizeof longline - 1] = '\0';
    push_str(&c, longline); /* -> slot 0 */
    char  *p0     = c.log.sb_lines[0];
    size_t p0_cap = malloc_usable_size(p0);
    ASSERT(p0 != NULL, "slot 0 allocated");

    SECTION("fill to capacity, then wrap");
    /* lines #1 .. #(CAP-1) are short — fills the ring exactly. */
    for (int i = 1; i < ZT_SCROLLBACK_CAP; i++) {
        char b[24];
        snprintf(b, sizeof b, "n%d", i);
        push_str(&c, b);
    }
    ASSERT(c.log.sb_count == ZT_SCROLLBACK_CAP, "ring saturated at CAP");
    ASSERT(c.log.sb_head == 0, "head still 0 at exactly CAP lines");
    ASSERT(strcmp(line_at(&c, ZT_SCROLLBACK_CAP - 1), longline) == 0,
           "oldest is the seed line");

    /* Push one more: evicts the oldest (slot 0) and wraps head. slot 0 holds a
     * short line now — must reuse p0 in place (fits) with no realloc. */
    push_str(&c, "wrapped");
    ASSERT(c.log.sb_count == ZT_SCROLLBACK_CAP, "count stays at CAP after wrap");
    ASSERT(c.log.sb_head == 1, "head advanced by one on eviction");
    ASSERT(c.log.sb_lines[0] == p0, "short line reused slot 0 buffer in place (no realloc)");
    ASSERT(malloc_usable_size(c.log.sb_lines[0]) == p0_cap,
           "slot 0 capacity unchanged on reuse");
    ASSERT(strcmp(line_at(&c, 0), "wrapped") == 0, "newest after wrap is the wrapped line");
    ASSERT(strlen(line_at(&c, 0)) == 7,
           "reused slot reads back clean — no leftover bytes past NUL");

    SECTION("grow: a longer line reallocs its slot");
    /* Next push reuses slot 1 (held "n1"); a long line must grow it. */
    char grow[300];
    memset(grow, 'g', sizeof grow - 1);
    grow[sizeof grow - 1] = '\0';
    push_str(&c, grow);
    ASSERT(strcmp(line_at(&c, 0), grow) == 0, "grown line stored correctly");
    ASSERT(malloc_usable_size(c.log.sb_lines[1]) >= sizeof grow,
           "slot 1 grew to fit the long line");

    scrollback_free(&c);
    ASSERT(c.log.sb_lines == NULL, "scrollback_free nulls the ring");

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "%d passed, %d failed\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");
    return g_fail == 0 ? 0 : 1;
}
