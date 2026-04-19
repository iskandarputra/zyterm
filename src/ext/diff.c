/**
 * @file diff.c
 * @brief Differential replay — compare two captured log files.
 *
 * Reads two zyterm RAW or TEXT log files and prints a unified diff-like
 * summary of the first N mismatching lines. Useful for regression
 * triage after firmware updates.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <stdio.h>
#include <string.h>

int diff_run(const char *a, const char *b) {
    if (!a || !b) return -1;
    FILE *fa = fopen(a, "r");
    if (!fa) {
        fprintf(stderr, "diff: open %s failed\n", a);
        return -1;
    }
    FILE *fb = fopen(b, "r");
    if (!fb) {
        fclose(fa);
        fprintf(stderr, "diff: open %s failed\n", b);
        return -1;
    }
    char la[4096], lb[4096];
    int  line = 0, mismatches = 0;
    while (1) {
        char *ra = fgets(la, sizeof la, fa);
        char *rb = fgets(lb, sizeof lb, fb);
        if (!ra && !rb) break;
        line++;
        if (!ra) {
            printf("+  %5d: %s", line, lb);
            mismatches++;
            continue;
        }
        if (!rb) {
            printf("-  %5d: %s", line, la);
            mismatches++;
            continue;
        }
        if (strcmp(la, lb) != 0) {
            printf("-  %5d: %s", line, la);
            printf("+  %5d: %s", line, lb);
            mismatches++;
        }
    }
    fclose(fa);
    fclose(fb);
    printf("%d mismatch(es) across %d line(s)\n", mismatches, line);
    return mismatches;
}
