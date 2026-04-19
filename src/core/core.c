/**
 * @file core.c
 * @brief Cross-cutting helpers: globals, I/O, output buffer, signals, terminal.
 *
 * This translation unit owns:
 *   - The process-wide global flags (zt_g_quit, zt_g_winch, …)
 *   - The stdout output buffer (ob_write / ob_cstr / ob_flush)
 *   - Safe I/O helpers (zt_write_all, zt_write_cstr, zt_warn, zt_die)
 *   - Monotonic time helpers (now, ts_diff_sec)
 *   - Signal handlers and signal installation (install_signals, sig_winch)
 *   - Terminal raw-mode management (setup_stdin_raw, restore_terminal)
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"
#include "zyterm.h"

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* =========================================================================
 * Process-wide globals (declared extern in zt_ctx.h)
 * ========================================================================= */

volatile sig_atomic_t zt_g_quit  = 0;
volatile sig_atomic_t zt_g_winch = 0;

struct termios        zt_g_orig_stdin;
int                   zt_g_orig_stdin_fl = 0;
bool                  zt_g_stdin_saved   = false;
bool                  zt_g_ui_active     = false;
bool                  zt_g_embedded      = false;

/* When zyterm runs embedded inside zy, fatal paths (zt_die / sig_crash)
 * must NOT call exit()/_exit() — that would terminate the host shell and
 * close the user's terminal window. Instead, the host arms this jump
 * buffer via zt_embed_arm() before calling zyterm_main(); the fatal
 * paths then siglongjmp back to it with a non-zero status. */
sigjmp_buf zt_g_embed_jmp;
bool       zt_g_embed_jmp_armed = false;

void       zt_embed_disarm(void) {
    zt_g_embed_jmp_armed = false;
}

/* Forward declaration — defined further below in this file. */
void        uninstall_signals(void);
static void zt_embed_reset_buffers(void);

/* ── Optional embed-mode trace log ───────────────────────────────────
 * When zyterm runs as a zy builtin, fatal exits would normally take the
 * host shell down. Trace is enabled by EITHER:
 *   1. ZYTERM_TRACE=/path/to/file in the process environment, OR
 *   2. The sentinel file /tmp/zyterm.trace existing — appendable
 *      without going through zy's `export` (which doesn't propagate to
 *      builtins). Trace records accumulate into that same file.
 * Best-effort, never fails. */
#define ZT_TRACE_SENTINEL "/tmp/zyterm.trace"
void zt_trace(const char *fmt, ...) {
    const char *path = getenv("ZYTERM_TRACE");
    if (!path || !*path) {
        if (access(ZT_TRACE_SENTINEL, F_OK) != 0) return;
        path = ZT_TRACE_SENTINEL;
    }
    FILE *f = fopen(path, "a");
    if (!f) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(f, "[%ld.%06ld pid=%d] ", (long)ts.tv_sec, (long)(ts.tv_nsec / 1000),
            (int)getpid());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void zt_embed_reset(void) {
    zt_trace("zt_embed_reset: stdin_saved=%d ui_active=%d quit=%d winch=%d "
             "handlers_saved=will-uninstall",
             zt_g_stdin_saved, zt_g_ui_active, (int)zt_g_quit, (int)zt_g_winch);
    /* Drop any signal handlers a prior embedded run left installed; this
     * is a no-op if nothing is currently saved. */
    uninstall_signals();
    /* The flags below are sticky across runs because zyterm normally
     * lives until process exit. When embedded, every invocation must
     * start from the same baseline as the very first call.
     *
     * restore_terminal() clears ui_active / stdin_saved on the way out,
     * but only if stdin was actually saved (i.e. setup_stdin_raw ran
     * successfully). When embedded with a non-tty stdin (script, pipe,
     * or zy's own pty in some test harnesses), setup_stdin_raw is a
     * no-op yet apply_layout still flips ui_active=true. Then on exit
     * restore_terminal() early-returns and ui_active stays true,
     * poisoning the next embedded invocation. Zero it here to be safe. */
    zt_g_quit      = 0;
    zt_g_winch     = 0;
    zt_g_ui_active = false;
    /* setup_stdin_raw() / restore_terminal() use this flag to decide
     * whether to snapshot or restore. After a clean run it is already
     * false; after a crash or longjmp it may still be true and would
     * cause the next setup_stdin_raw() to skip its work. */
    zt_g_stdin_saved = false;
    /* Discard any leftover bytes in the render buffer so they aren't
     * spuriously emitted on the host's terminal at the next ob_flush. */
    zt_embed_reset_buffers();
    /* Scrub file-static state in other subsystems that would otherwise
     * persist across embedded invocations. These are no-ops if the
     * feature wasn't used in the previous run. */
    multi_embed_reset();
    session_embed_reset();
}

/* =========================================================================
 * Stdout output buffer
 *
 * All rendering code accumulates output here and calls ob_flush() once per
 * frame. This avoids many small write(2) syscalls and produces flicker-free
 * screen updates.
 * ========================================================================= */

#define OB_CAP (256 * 1024) /* 256 KiB — sized for a single full-screen frame */

static unsigned char s_ob[OB_CAP];
static size_t        s_ob_len = 0;

void                 ob_write(const void *p, size_t n) {
    if (!p || n == 0) return;
    if (s_ob_len + n > OB_CAP) {
        /* Flush what we have, then write the rest directly. */
        ob_flush();
        if (n > OB_CAP) {
            (void)zt_write_all(STDOUT_FILENO, p, n);
            return;
        }
    }
    memcpy(s_ob + s_ob_len, p, n);
    s_ob_len += n;
}

void ob_cstr(const char *s) {
    if (s) ob_write(s, strlen(s));
}

void ob_flush(void) {
    if (s_ob_len == 0) return;
    (void)zt_write_all(STDOUT_FILENO, s_ob, s_ob_len);
    s_ob_len = 0;
}

/* Discard any buffered render output without emitting it. Used by
 * zt_embed_reset() to ensure leftover bytes from a previous embedded
 * run can't bleed onto the host's terminal. */
static void zt_embed_reset_buffers(void) {
    s_ob_len = 0;
}

/* =========================================================================
 * Safe I/O helpers
 * ========================================================================= */

int zt_write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p         = (const unsigned char *)buf;
    size_t               remaining = n;
    while (remaining > 0) {
        ssize_t w = write(fd, p, remaining);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)w;
        remaining -= (size_t)w;
    }
    return 0;
}

int zt_write_cstr(int fd, const char *s) {
    if (!s) return 0;
    return zt_write_all(fd, s, strlen(s));
}

void zt_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void zt_die(const char *fmt, ...) {
    char    msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", msg);
    zt_trace("zt_die: embedded=%d armed=%d msg=%s", zt_g_embedded, zt_g_embed_jmp_armed, msg);
    /* Embedded-in-zy: bail out to the host instead of killing the shell. */
    if (zt_g_embedded && zt_g_embed_jmp_armed) {
        zt_g_embed_jmp_armed = false;
        siglongjmp(zt_g_embed_jmp, 1);
    }
    exit(1);
}

/* =========================================================================
 * Monotonic time helpers
 * ========================================================================= */

void now(struct timespec *t) {
    clock_gettime(CLOCK_MONOTONIC, t);
}

double ts_diff_sec(const struct timespec *a, const struct timespec *b) {
    /* Returns a - b in seconds. */
    return (double)(a->tv_sec - b->tv_sec) + (double)(a->tv_nsec - b->tv_nsec) * 1e-9;
}

/* =========================================================================
 * Signal handlers
 * ========================================================================= */

static void sig_quit(int s) {
    (void)s;
    zt_g_quit = 1;
}

void sig_winch(int s) {
    (void)s;
    zt_g_winch = 1;
}

/**
 * Emergency terminal restore for fatal signals (SIGSEGV, SIGABRT, …).
 *
 * Uses only async-signal-safe functions: tcsetattr, write, _exit.
 * SA_RESETHAND ensures the handler fires at most once — re-raising the
 * signal afterwards lets the OS generate a core dump normally.
 */
static void sig_crash(int s) {
    /* zt_trace not async-signal-safe; we only call it on the embedded
     * fast-path where we are about to siglongjmp anyway. The standalone
     * path stays purely async-signal-safe. */
    if (zt_g_embedded && zt_g_embed_jmp_armed)
        zt_trace("sig_crash: signal=%d (embedded, will siglongjmp)", s);
    if (zt_g_stdin_saved) tcsetattr(STDIN_FILENO, TCSANOW, &zt_g_orig_stdin);
    /* Async-signal-safe emergency cleanup. Same reset ordering as
     * restore_terminal() — reset scroll region on alt-screen first,
     * then leave alt-screen, then reset state on the main buffer. */
    static const char cleanup[]        = "\033[r"
                                         "\033[?1049l"
                                         "\033[?1006l\033[?1002l\033[?1000l\033[?1004l"
                                         "\033[?2004l"
                                         "\033[?7h\033[?25h\033[?12h\033[0 q"
                                         "\033>\033(B\033[m";
    ssize_t wr __attribute__((unused)) = write(STDOUT_FILENO, cleanup, sizeof cleanup - 1);
    /* Embedded-in-zy: jump back to the host instead of dying — the host
     * will surface the crash as a non-zero builtin status and stay alive.
     * siglongjmp from a signal handler is async-signal-safe (POSIX). */
    if (zt_g_embedded && zt_g_embed_jmp_armed) {
        zt_g_embed_jmp_armed = false;
        siglongjmp(zt_g_embed_jmp, 128 + s);
    }
    raise(s); /* SA_RESETHAND already reset to SIG_DFL → core dump */
    _exit(128 + s);
}

void install_signals(void);
void uninstall_signals(void);

/* Saved handlers for restore on uninstall (embedded-in-zy scenario).
 * Populated by install_signals(), consumed by uninstall_signals(). */
static struct sigaction s_prev_int, s_prev_term, s_prev_hup, s_prev_quit;
static struct sigaction s_prev_winch, s_prev_pipe;
static struct sigaction s_prev_segv, s_prev_abrt, s_prev_bus, s_prev_fpe;
static bool             s_handlers_saved = false;

void                    install_signals(void) {
    zt_trace("install_signals: already_saved=%d", s_handlers_saved);
    /* Idempotent: a second call would otherwise capture our own handlers
     * as "previous", losing the host's originals. */
    if (s_handlers_saved) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = sig_quit;
    sigaction(SIGINT, &sa, &s_prev_int);
    sigaction(SIGTERM, &sa, &s_prev_term);
    sigaction(SIGHUP, &sa, &s_prev_hup);
    sigaction(SIGQUIT, &sa, &s_prev_quit);

    sa.sa_handler = sig_winch;
    sigaction(SIGWINCH, &sa, &s_prev_winch);

    /* Ignore SIGPIPE — we handle write errors explicitly. */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, &s_prev_pipe);

    /* Crash signals: restore terminal then re-raise for core dump. */
    struct sigaction crash_sa;
    memset(&crash_sa, 0, sizeof crash_sa);
    sigemptyset(&crash_sa.sa_mask);
    crash_sa.sa_handler = sig_crash;
    crash_sa.sa_flags   = SA_RESETHAND; /* one-shot: re-raise hits SIG_DFL */
    sigaction(SIGSEGV, &crash_sa, &s_prev_segv);
    sigaction(SIGABRT, &crash_sa, &s_prev_abrt);
    sigaction(SIGBUS, &crash_sa, &s_prev_bus);
    sigaction(SIGFPE, &crash_sa, &s_prev_fpe);

    s_handlers_saved = true;
}

/** Restore the handlers that were active before install_signals().
 *  Safe to call when not installed — it's a no-op in that case. */
void uninstall_signals(void) {
    zt_trace("uninstall_signals: handlers_saved=%d", s_handlers_saved);
    if (!s_handlers_saved) return;
    sigaction(SIGINT, &s_prev_int, NULL);
    sigaction(SIGTERM, &s_prev_term, NULL);
    sigaction(SIGHUP, &s_prev_hup, NULL);
    sigaction(SIGQUIT, &s_prev_quit, NULL);
    sigaction(SIGWINCH, &s_prev_winch, NULL);
    sigaction(SIGPIPE, &s_prev_pipe, NULL);
    sigaction(SIGSEGV, &s_prev_segv, NULL);
    sigaction(SIGABRT, &s_prev_abrt, NULL);
    sigaction(SIGBUS, &s_prev_bus, NULL);
    sigaction(SIGFPE, &s_prev_fpe, NULL);
    s_handlers_saved = false;
}

/* =========================================================================
 * Terminal management
 * ========================================================================= */

void setup_stdin_raw(void) {
    if (zt_g_stdin_saved) return;

    if (tcgetattr(STDIN_FILENO, &zt_g_orig_stdin) != 0) return;
    zt_g_orig_stdin_fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    zt_g_stdin_saved   = true;

    struct termios raw = zt_g_orig_stdin;
    /* Input: no break signal, no CR→NL, no parity strip, no flow control,
     * 8-bit clean. */
    raw.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /* Output: pass through as-is (we emit ANSI ourselves). */
    raw.c_oflag &= (tcflag_t) ~(OPOST);
    /* Character size 8. */
    raw.c_cflag |= CS8;
    /* Local: no echo, no canonical, no signals from keys, no extended. */
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
    /* read() returns immediately with whatever is available. */
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void restore_terminal(void) {
    /* zt_g_ui_active can be true even when stdin_saved is false: that
     * happens when run_interactive flipped the UI-on flag but stdin
     * was not a tty (embedded under a pipe/non-tty). In that case we
     * still need to emit the "leave alt-screen / mouse off" escapes
     * so the host terminal isn't left in zyterm's UI mode. The
     * termios restore below is properly gated on stdin_saved. */
    if (!zt_g_stdin_saved && !zt_g_ui_active) return;
    /* Tell the terminal to stop emitting mouse/bracketed-paste/etc.
     * events FIRST, then drain any already-queued bytes via TCSAFLUSH.
     * If we did it the other way around, mouse movements that happen
     * between tcsetattr and the write() below would inject escape
     * sequences into stdin — which zy's parent line editor then sees
     * as keystrokes, causing a prompt-spam loop. */
    if (zt_g_ui_active) {
        /* Order matters: reset scroll region BEFORE leaving alt-screen.
         * Per the VT100 spec, DECSTBM (\033[r) unconditionally moves the
         * cursor to the home position (row 1, col 1). If sent after
         * \033[?1049l, that cursor-homing side effect lands on the main
         * buffer and clobbers the saved cursor position — the parent
         * shell's prompt then appears at the top of the screen instead
         * of where the user typed the command. Sending \033[r while
         * still on the alt buffer confines the side effect there (we're
         * about to discard it anyway), and \033[?1049l then cleanly
         * restores the main buffer + cursor.
         *
         * Reset inventory:
         *   r       reset scroll region (DECSTBM) — on alt screen
         *   ?1049l  leave alt screen (restores main buffer + cursor)
         *   ?1006l  SGR mouse  off
         *   ?1002l  button-event mouse off
         *   ?1000l  basic mouse off
         *   ?1004l  focus in/out events off
         *   ?2004l  bracketed paste off
         *   ?7h     autowrap on (default)
         *   ?25h    cursor visible
         *   ?12h    re-enable cursor blink (we disabled it on entry)
         *   0 q     DECSCUSR "reset to terminal default cursor style"
         *   >       keypad numeric mode (not application)
         *   (B      select ASCII charset for G0 (no DEC line-drawing)
         *   [m      reset SGR attributes */
        (void)zt_write_cstr(STDOUT_FILENO, "\033[r"
                                           "\033[?1049l"
                                           "\033[?1006l\033[?1002l\033[?1000l\033[?1004l"
                                           "\033[?2004l"
                                           "\033[?7h\033[?25h\033[?12h\033[0 q"
                                           "\033>"
                                           "\033(B"
                                           "\033[m");
        zt_g_ui_active = false;
    }
    /* TCSAFLUSH waits for pending output to drain AND discards any
     * still-queued input — so the "stop mouse" escape above is delivered
     * before we drop any straggler mouse-event bytes. Only do this if
     * we actually snapshotted the original state. */
    if (zt_g_stdin_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &zt_g_orig_stdin);
        if (zt_g_orig_stdin_fl >= 0) fcntl(STDIN_FILENO, F_SETFL, zt_g_orig_stdin_fl);
        zt_g_stdin_saved = false;
    }
}
