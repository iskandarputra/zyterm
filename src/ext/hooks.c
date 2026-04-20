/**
 * @file hooks.c
 * @brief Event hooks — react to RX patterns and connection events.
 *
 * Three hook event kinds:
 *
 *   --on-match  '/PATTERN/=ACTION'   fires per RX line matching PATTERN
 *   --on-connect    'ACTION'         fires after device opens
 *   --on-disconnect 'ACTION'         fires when device closes
 *
 * ACTION dispatch:
 *   * "send:..."   inject bytes onto the wire (escapes resolved via
 *                  expand_escapes — same dialect as F-key macros:
 *                  \\r, \\n, \\t, \\xNN, \\NNN).
 *   * anything else  fork + execve "/bin/sh -c <cmd>" with stdin
 *                  closed, stdout/stderr inherited (so output lands
 *                  on the user's terminal as "[hook] ..." after the
 *                  shell's own write). Non-blocking — we don't wait;
 *                  the SIGCHLD handler / hooks_reap() collects exit
 *                  codes opportunistically.
 *
 * Environment passed to shelled-out hooks:
 *   ZYTERM_PORT   the device path or URL we're connected to
 *   ZYTERM_BAUD   serial baud (decimal; "0" for socket transports)
 *   ZYTERM_LINE   the matched RX line (MATCH event only)
 *   ZYTERM_MATCH  the regex's first capture group, or the full match
 *
 * Rate-limit: each hook fires at most once per 100 ms to keep a
 * device that endlessly spams "PANIC" from forking thousands of
 * children.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ZT_HOOK_MAX             16
#define ZT_HOOK_RATE_LIMIT_MS   100
#define ZT_HOOK_PIDS_MAX        32

typedef struct {
    int             event;          /* ZT_HOOK_EVENT_* */
    bool            in_use;
    bool            send;           /* true if action begins with "send:" */
    bool            has_regex;
    regex_t         regex;
    char           *pattern_src;    /* original regex text, for logs / env */
    char           *action;         /* the part after "send:" (if any) */
    struct timespec t_last_fire;
} hook_t;

typedef struct {
    hook_t hooks[ZT_HOOK_MAX];
    int    count;
    pid_t  pids[ZT_HOOK_PIDS_MAX]; /* outstanding shelled-out children */
    int    pids_count;
} hooks_state;

extern void direct_send(zt_ctx *c, const unsigned char *buf, size_t n);

/* ── helpers ───────────────────────────────────────────────────────────── */

static hooks_state *get_state(zt_ctx *c) {
    if (!c->ext.hooks) c->ext.hooks = calloc(1, sizeof(hooks_state));
    return (hooks_state *) c->ext.hooks;
}

static long ms_since(const struct timespec *prev, const struct timespec *now_ts) {
    return (now_ts->tv_sec - prev->tv_sec) * 1000L
         + (now_ts->tv_nsec - prev->tv_nsec) / 1000000L;
}

static void track_pid(hooks_state *st, pid_t pid) {
    if (st->pids_count < ZT_HOOK_PIDS_MAX) st->pids[st->pids_count++] = pid;
    /* If full, the child becomes a zombie until exit — cosmetic only. */
}

static void run_shell_action(zt_ctx *c, const hook_t *h,
                             const unsigned char *line, size_t line_len) {
    pid_t pid = fork();
    if (pid < 0) {
        log_notice(c, "hook: fork failed: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        /* child */
        char baud_s[16];
        snprintf(baud_s, sizeof baud_s, "%u", c->serial.baud);
        setenv("ZYTERM_PORT", c->serial.device ? c->serial.device : "", 1);
        setenv("ZYTERM_BAUD", baud_s, 1);
        if (line && line_len > 0) {
            /* NUL-terminate by truncating to a stack buf (1 KiB cap) */
            char buf[1024];
            size_t n = line_len < sizeof buf - 1 ? line_len : sizeof buf - 1;
            memcpy(buf, line, n);
            buf[n] = '\0';
            setenv("ZYTERM_LINE", buf, 1);
            setenv("ZYTERM_MATCH", buf, 1); /* full match; group 1 left for v2 */
        }
        if (h->pattern_src) setenv("ZYTERM_PATTERN", h->pattern_src, 1);
        /* Detach from controlling tty's stdin so the hook can't grab
         * input meant for zyterm. stdout/stderr stay attached so any
         * shell output appears in the user's terminal. */
        int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        execl("/bin/sh", "sh", "-c", h->action, (char *) NULL);
        _exit(127);
    }
    track_pid(get_state(c), pid);
}

static void run_send_action(zt_ctx *c, const hook_t *h) {
    char   buf[1024];
    size_t n = expand_escapes(h->action, buf, sizeof buf);
    if (n > 0) direct_send(c, (const unsigned char *) buf, n);
}

static void fire_hook(zt_ctx *c, hook_t *h,
                      const unsigned char *line, size_t line_len) {
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    if (h->t_last_fire.tv_sec != 0
        && ms_since(&h->t_last_fire, &now_ts) < ZT_HOOK_RATE_LIMIT_MS) {
        return; /* rate-limited */
    }
    h->t_last_fire = now_ts;

    if (h->send) run_send_action(c, h);
    else         run_shell_action(c, h, line, line_len);
}

/* ── parse "/regex/=action" or just "action" ───────────────────────────── */

static int parse_match_spec(const char *spec, char **regex_out, char **action_out) {
    if (spec[0] != '/') return -1;
    /* find "/=" delimiter, honouring backslash escapes (\/ inside pattern) */
    const char *p = spec + 1;
    const char *end = NULL;
    while (*p) {
        if (*p == '\\' && p[1]) { p += 2; continue; }
        if (*p == '/' && p[1] == '=') { end = p; break; }
        p++;
    }
    if (!end) return -1;

    size_t rlen = (size_t)(end - (spec + 1));
    *regex_out  = strndup(spec + 1, rlen);
    *action_out = strdup(end + 2);
    if (!*regex_out || !*action_out) {
        free(*regex_out); free(*action_out);
        return -1;
    }
    return 0;
}

/* ── public API ────────────────────────────────────────────────────────── */

int hooks_register(zt_ctx *c, int event, const char *spec) {
    if (!c || !spec) return -1;
    hooks_state *st = get_state(c);
    if (st->count >= ZT_HOOK_MAX) {
        log_notice(c, "hook: table full (max %d)", ZT_HOOK_MAX);
        return -1;
    }
    hook_t *h = &st->hooks[st->count];
    memset(h, 0, sizeof *h);
    h->event = event;

    char *action = NULL;
    if (event == ZT_HOOK_EVENT_MATCH) {
        if (parse_match_spec(spec, &h->pattern_src, &action) != 0) {
            log_notice(c, "hook: --on-match expects /PATTERN/=ACTION (got: %s)", spec);
            return -1;
        }
        if (regcomp(&h->regex, h->pattern_src, REG_EXTENDED | REG_NEWLINE) != 0) {
            log_notice(c, "hook: bad regex: %s", h->pattern_src);
            free(h->pattern_src); free(action);
            return -1;
        }
        h->has_regex = true;
    } else {
        action = strdup(spec);
        if (!action) return -1;
    }

    if (strncmp(action, "send:", 5) == 0) {
        h->send   = true;
        h->action = strdup(action + 5);
        free(action);
    } else {
        h->action = action;
    }
    h->in_use = true;
    st->count++;
    return 0;
}

void hooks_on_line(zt_ctx *c, const unsigned char *line, size_t len) {
    if (!c || !c->ext.hooks || !line || len == 0) return;
    hooks_state *st = (hooks_state *) c->ext.hooks;

    /* regexec needs a NUL-terminated string; cap at 4 KiB which
     * covers any realistic single line of serial output. */
    char buf[4096];
    size_t n = len < sizeof buf - 1 ? len : sizeof buf - 1;
    memcpy(buf, line, n);
    buf[n] = '\0';

    for (int i = 0; i < st->count; i++) {
        hook_t *h = &st->hooks[i];
        if (!h->in_use || h->event != ZT_HOOK_EVENT_MATCH) continue;
        int rr = regexec(&h->regex, buf, 0, NULL, 0);
        if (rr == 0) fire_hook(c, h, line, len);
    }
}

void hooks_on_event(zt_ctx *c, int event) {
    if (!c || !c->ext.hooks) return;
    hooks_state *st = (hooks_state *) c->ext.hooks;
    for (int i = 0; i < st->count; i++) {
        hook_t *h = &st->hooks[i];
        if (h->in_use && h->event == event) fire_hook(c, h, NULL, 0);
    }
}

void hooks_reap(zt_ctx *c) {
    if (!c || !c->ext.hooks) return;
    hooks_state *st = (hooks_state *) c->ext.hooks;
    for (int i = 0; i < st->pids_count;) {
        int   status = 0;
        pid_t r      = waitpid(st->pids[i], &status, WNOHANG);
        if (r == 0) { i++; continue; } /* still running */
        /* exited or error — drop slot via swap-pop */
        st->pids[i] = st->pids[--st->pids_count];
    }
}

void hooks_free(zt_ctx *c) {
    if (!c || !c->ext.hooks) return;
    hooks_state *st = (hooks_state *) c->ext.hooks;
    for (int i = 0; i < st->count; i++) {
        hook_t *h = &st->hooks[i];
        if (h->has_regex) regfree(&h->regex);
        free(h->pattern_src);
        free(h->action);
    }
    /* Reap any still-running shelled-out children once, non-blocking. */
    for (int i = 0; i < st->pids_count; i++) {
        int s;
        (void) waitpid(st->pids[i], &s, WNOHANG);
    }
    free(st);
    c->ext.hooks = NULL;
}
