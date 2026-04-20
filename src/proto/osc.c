/**
 * @file osc.c
 * @brief OSC 52 clipboard + OSC 8 hyperlink helpers.
 *
 * OSC 52 lets the terminal emulator place arbitrary text on the system
 * clipboard via `ESC ] 52 ; c ; <base64> BEL`. Used for "copy match"
 * (Ctrl+A Y) when the user has `osc52_enabled == true`.
 *
 * OSC 8 wraps URL-like substrings in
 *   `ESC ] 8 ; ; <url> ST <text> ESC ] 8 ; ; ST`
 * so that modern terminals render them as clickable hyperlinks. We keep
 * a simple scanner that matches `http://`, `https://`, and `file://`.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Try to pipe `n` bytes of `buf` into a system clipboard helper.
 * Returns the helper name on success, NULL if no helper worked. We try
 * Wayland (wl-copy) first, then X11 (xclip, xsel), then macOS (pbcopy).
 * This is a hard fallback for terminals/multiplexers that drop OSC 52
 * (gnome-terminal, default kitty, default xterm, tmux without
 * `set-clipboard on`, screen, …).
 *
 * Environment-aware: skip helpers that need a display server we don't
 * have — avoids wasteful fork+exec on pure-Wayland (no xclip) or
 * headless (no wl-copy) systems. */
static const char *clipboard_pipe(const char *buf, size_t n) {
    const char *wl_disp = getenv("WAYLAND_DISPLAY");
    const char *x_disp  = getenv("DISPLAY");
    static const struct {
        const char *name;
        const char *argv[4];
        int         env; /* 0 = any, 1 = WAYLAND_DISPLAY, 2 = DISPLAY */
    } helpers[] = {
        {"wl-copy", {"wl-copy", NULL, NULL, NULL}, 1},
        {"xclip",   {"xclip", "-selection", "clipboard", NULL}, 2},
        {"xsel",    {"xsel", "--clipboard", "--input", NULL}, 2},
        {"pbcopy",  {"pbcopy", NULL, NULL, NULL}, 0},
    };
    for (size_t h = 0; h < sizeof helpers / sizeof helpers[0]; h++) {
        if (helpers[h].env == 1 && (!wl_disp || !*wl_disp)) continue;
        if (helpers[h].env == 2 && (!x_disp  || !*x_disp))  continue;
        int fds[2];
        if (pipe(fds) < 0) continue;
        pid_t pid = fork();
        if (pid < 0) {
            close(fds[0]);
            close(fds[1]);
            continue;
        }
        if (pid == 0) {
            /* child: stdin = read end, silence stdout/stderr */
            dup2(fds[0], STDIN_FILENO);
            close(fds[0]);
            close(fds[1]);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > STDERR_FILENO) close(devnull);
            }
            execvp(helpers[h].argv[0], (char *const *)helpers[h].argv);
            _exit(127);
        }
        close(fds[0]);
        /* Ignore SIGPIPE in case the helper exits early. */
        struct sigaction old_pipe, ign = {0};
        ign.sa_handler = SIG_IGN;
        sigaction(SIGPIPE, &ign, &old_pipe);
        size_t off = 0;
        while (off < n) {
            ssize_t w = write(fds[1], buf + off, n - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                break;
            }
            off += (size_t)w;
        }
        close(fds[1]);
        sigaction(SIGPIPE, &old_pipe, NULL);
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) continue;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && off == n) return helpers[h].name;
        /* exec failed (127) or helper rejected — try next */
    }
    return NULL;
}

static void b64enc(const unsigned char *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned)in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? B64[v & 63] : '=';
    }
    out[o] = '\0';
}

/* Tier 4 — file-based clipboard fallback.
 *
 * Absolute last resort when no system clipboard path succeeded.
 * Writes the selection to $XDG_CACHE_HOME/zyterm/clipboard
 * (default ~/.cache/zyterm/clipboard). File is overwritten on
 * each copy — no unbounded growth. Permissions 0600 for privacy.
 *
 * This guarantees the user never loses a selection: even on a
 * headless SSH session or a locked-down Wayland compositor with
 * no helpers installed, the text lands somewhere retrievable
 * (cat, xargs, pipe into another tool). */
static const char *clipboard_file(const char *buf, size_t n) {
    static char fpath[512];
    const char *cache = getenv("XDG_CACHE_HOME");
    const char *home  = getenv("HOME");

    if (cache && *cache) {
        snprintf(fpath, sizeof fpath, "%s/zyterm", cache);
    } else if (home && *home) {
        snprintf(fpath, sizeof fpath, "%s/.cache", home);
        (void)mkdir(fpath, 0700);
        snprintf(fpath, sizeof fpath, "%s/.cache/zyterm", home);
    } else {
        snprintf(fpath, sizeof fpath, "/tmp/zyterm-%d", (int)getpid());
    }
    (void)mkdir(fpath, 0700);

    size_t dlen = strlen(fpath);
    snprintf(fpath + dlen, sizeof fpath - dlen, "/clipboard");

    int fd = open(fpath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return NULL;

    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return NULL;
        }
        off += (size_t)w;
    }
    close(fd);
    return fpath;
}

/* ── Unified clipboard push ─────────────────────────────────────────────── *
 *
 * Four-tier cascade — every selection always lands somewhere:
 *
 *   Tier 1  Native X11 selection owner (xcb, optional build-time).
 *           Works on X11 and Wayland+XWayland when $DISPLAY is set.
 *
 *   Tier 2  OSC 52 terminal escape (always emitted when enabled).
 *           The host terminal places the text on the system clipboard.
 *           Ghostty, Alacritty, kitty, foot, wezterm, iTerm2, etc.
 *
 *   Tier 3  External helper binary (wl-copy / xclip / xsel / pbcopy).
 *           Only tried when Tier 1 didn't take ownership.
 *           Environment-aware: skips helpers that need a display
 *           server we don't have.
 *
 *   Tier 4  File fallback (~/.cache/zyterm/clipboard).
 *           Fires only when Tiers 1+3 both failed. Guarantees the
 *           user never loses a selection, even headless/SSH.
 *
 * The flash message tells the user exactly which tier succeeded and
 * what to install to get a better path next time.                        */
void osc52_copy(zt_ctx *c, const char *buf, size_t n) {
    if (!c || !buf || n == 0) return;
    if (n > 100000) n = 100000;

    /* ── Tier 1: native X11 selection owner ──────────────────────────── */
    bool sys_ok = clipboard_native_set(buf, n);

    /* ── Tier 2: OSC 52 escape (always, when enabled) ────────────────── */
    bool osc52_sent = false;
    if (c->proto.osc52_enabled) {
        char *b64 = malloc(4 * ((n + 2) / 3) + 1);
        if (b64) {
            b64enc((const unsigned char *)buf, n, b64);
            ob_cstr("\x1b]52;c;");
            ob_cstr(b64);
            ob_cstr("\x07");
            ob_flush();
            free(b64);
            osc52_sent = true;
        }
    }

    /* ── Tier 3: external helper ─────────────────────────────────────── */
    if (!sys_ok) {
        const char *h = clipboard_pipe(buf, n);
        if (h) {
            set_flash(c, "copied %zu bytes via %s", n, h);
            return;
        }
    }

    /* ── Tier 4: file fallback ───────────────────────────────────────── */
    const char *fpath = NULL;
    if (!sys_ok)
        fpath = clipboard_file(buf, n);

    /* ── Status flash ────────────────────────────────────────────────── */
    if (sys_ok) {
        set_flash(c, "copied %zu bytes (native X11 owner)", n);
    } else if (fpath && osc52_sent) {
        set_flash(c, "copied %zu bytes \xc2\xb7 OSC 52 + %s", n, fpath);
    } else if (fpath) {
        set_flash(c, "copied %zu bytes \xe2\x86\x92 %s", n, fpath);
    } else if (osc52_sent) {
        set_flash(c, "copied %zu bytes via OSC 52", n);
    } else {
        set_flash(c, "copy failed \xe2\x80\x94 no clipboard backend");
    }
}

size_t osc8_rewrite(const unsigned char *in, size_t n, unsigned char *out, size_t cap) {
    if (!in || !out || cap == 0) return 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        /* look for "http://" / "https://" / "file://" starts */
        if (o + 64 >= cap) break;
        int    is_url  = 0;
        size_t url_len = 0;
        if (i + 7 <= n && (!memcmp(in + i, "http://", 7) || !memcmp(in + i, "file://", 7)))
            is_url = 1;
        else if (i + 8 <= n && !memcmp(in + i, "https://", 8))
            is_url = 2;
        if (is_url) {
            size_t start = i;
            while (i < n && !isspace((unsigned char)in[i]) && in[i] != '"' && in[i] != '\'' &&
                   in[i] != '<' && in[i] != '>')
                i++;
            url_len = i - start;
            if (o + url_len + 32 >= cap) break;
            /* prefix OSC 8 ; ; url ST */
            o += (size_t)snprintf((char *)out + o, cap - o, "\x1b]8;;%.*s\x1b\\", (int)url_len,
                                  in + start);
            memcpy(out + o, in + start, url_len);
            o += url_len;
            /* suffix OSC 8 ; ; ST */
            const char *end = "\x1b]8;;\x1b\\";
            memcpy(out + o, end, 7);
            o += 7;
            i--; /* compensate for upcoming loop i++ */
        } else {
            out[o++] = in[i];
        }
    }
    return o;
}
