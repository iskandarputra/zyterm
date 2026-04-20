/**
 * @file record_cast.c
 * @brief Asciinema cast v2 session recorder.
 *
 * Captures every byte that leaves zyterm for the user's terminal (i.e.
 * after rendering, escape sequences and all) and writes them to disk in
 * the standard asciinema cast v2 format. The result plays back in
 * `asciinema play file.cast`, embeds in the asciinema web player, and
 * round-trips through tools like agg (cast -> GIF/SVG).
 *
 * Format (one JSON object per line):
 *   header:  {"version":2,"width":80,"height":24,"timestamp":1700000000,
 *             "env":{"TERM":"xterm-256color","SHELL":"/bin/bash"}}
 *   events:  [t_seconds, "o", "data string"]
 *
 * Tap point: ob_flush() in core.c calls cast_record_o() with the bytes
 * about to hit STDOUT, so what we record is exactly what the user sees.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Single-recording state. zyterm only ever runs one --rec at a time. */
static FILE          *s_fp    = NULL;
static struct timespec s_t0   = {0};

/* ── helpers ───────────────────────────────────────────────────────────── */

static double now_rel(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) (t.tv_sec - s_t0.tv_sec)
         + (double) (t.tv_nsec - s_t0.tv_nsec) / 1e9;
}

/* JSON-string-escape one byte into @c out (which must hold >= 8 bytes:
 * worst case is the 6-char "\uXXXX" plus the snprintf NUL terminator).
 * Returns the number of bytes written (NOT counting the NUL). */
static size_t json_escape_byte(unsigned char b, char *out) {
    switch (b) {
    case '"':  out[0] = '\\'; out[1] = '"';  return 2;
    case '\\': out[0] = '\\'; out[1] = '\\'; return 2;
    case '\b': out[0] = '\\'; out[1] = 'b';  return 2;
    case '\f': out[0] = '\\'; out[1] = 'f';  return 2;
    case '\n': out[0] = '\\'; out[1] = 'n';  return 2;
    case '\r': out[0] = '\\'; out[1] = 'r';  return 2;
    case '\t': out[0] = '\\'; out[1] = 't';  return 2;
    default:
        if (b < 0x20 || b == 0x7f) {
            int w = snprintf(out, 8, "\\u%04x", b);
            return (w > 0) ? (size_t) w : 0;
        }
        out[0] = (char) b;
        return 1;
    }
}

static void json_write_escaped(FILE *fp, const unsigned char *buf, size_t n) {
    char scratch[8];
    for (size_t i = 0; i < n; i++) {
        size_t w = json_escape_byte(buf[i], scratch);
        fwrite(scratch, 1, w, fp);
    }
}

static void terminal_size(int *cols, int *rows) {
    struct winsize w = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col && w.ws_row) {
        *cols = w.ws_col;
        *rows = w.ws_row;
    } else {
        *cols = 80;
        *rows = 24;
    }
}

/* ── public API ────────────────────────────────────────────────────────── */

int cast_record_open(zt_ctx *c, const char *path) {
    if (!path || !*path) {
        errno = EINVAL;
        return -1;
    }
    if (s_fp) cast_record_close(c);

    s_fp = fopen(path, "wb");
    if (!s_fp) return -1;

    /* Line-buffered so a Ctrl+C still leaves a parseable file on disk. */
    setvbuf(s_fp, NULL, _IOLBF, 0);

    int cols, rows;
    terminal_size(&cols, &rows);

    const char *term  = getenv("TERM")  ? getenv("TERM")  : "xterm-256color";
    const char *shell = getenv("SHELL") ? getenv("SHELL") : "/bin/sh";

    /* Header line. We escape TERM/SHELL just in case (paranoia for
     * unusual env values). */
    fputs("{\"version\":2,\"width\":", s_fp);
    fprintf(s_fp, "%d,\"height\":%d,\"timestamp\":%lld,",
            cols, rows, (long long) time(NULL));
    fputs("\"env\":{\"TERM\":\"", s_fp);
    json_write_escaped(s_fp, (const unsigned char *) term, strlen(term));
    fputs("\",\"SHELL\":\"", s_fp);
    json_write_escaped(s_fp, (const unsigned char *) shell, strlen(shell));
    fputs("\"},\"title\":\"zyterm " ZT_VERSION "\"}\n", s_fp);

    clock_gettime(CLOCK_MONOTONIC, &s_t0);
    if (c) c->log.rec_path = path;
    ob_set_record_callback(cast_record_o);
    return 0;
}

void cast_record_o(const unsigned char *buf, size_t n) {
    if (!s_fp || !buf || !n) return;

    fprintf(s_fp, "[%.6f, \"o\", \"", now_rel());
    json_write_escaped(s_fp, buf, n);
    fputs("\"]\n", s_fp);
}

void cast_record_close(zt_ctx *c) {
    if (!s_fp) return;
    ob_set_record_callback(NULL);
    fflush(s_fp);
    fclose(s_fp);
    s_fp = NULL;
    if (c) c->log.rec_path = NULL;
}
