/**
 * @file main.c
 * @brief CLI parsing + zyterm_main + main entry
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"
#include "zyterm.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------ CLI -------------------------------------- */

static void usage(const char *a0) {
    fprintf(stderr,
            "zyterm " ZT_VERSION " - high-performance RTOS serial terminal\n"
            "\n"
            "usage: %s [options] <device>\n"
            "       %s --replay <logfile>\n"
            "\n"
            "connection:\n"
            "  -b, --baud <rate>       baud rate (default 115200; arbitrary via termios2)\n"
            "      --data <5|6|7|8>    data bits (default 8)\n"
            "      --parity <n|e|o>    parity: none | even | odd (default n)\n"
            "      --stop <1|2>        stop bits (default 1)\n"
            "      --flow <n|r|x>      flow: none | rts/cts | xon/xoff (default n)\n"
            "      --reconnect         auto-reopen device on hang-up (default: ON)\n"
            "      --no-reconnect      exit on serial hang-up instead of retrying\n"
            "\n"
            "logging & capture:\n"
            "  -l, --log <file>        append log with ms-resolution timestamps\n"
            "                          (interactive: Ctrl+A l toggles auto-named log\n"
            "                          zyterm-YYYYMMDD-NNN.txt)\n"
            "      --log-max-kb <N>    rotate log to <file>.1 when it exceeds N KB\n"
            "      --tx-ts             also log TX stream with timestamps (-> prefix)\n"
            "      --dump <sec>        headless capture for N seconds (0 = forever)\n"
            "      --replay <file>     replay a capture file through the UI\n"
            "      --replay-speed <x>  replay speed multiplier (default 1.0, 0 = max)\n"
            "\n"
            "display & input:\n"
            "  -x, --hex               render RX as hex dump\n"
            "  -e, --echo              start with local echo on\n"
            "      --no-color          disable RX log-level coloring\n"
            "      --ts                start with timestamp display on\n"
            "      --watch <pattern>   highlight lines containing <pattern> (repeatable,\n"
            "                          up to 8, each gets a distinct colour)\n"
            "      --watch-beep        beep (BEL) on watch match\n"
            "      --macro F<n>=<str>  bind a macro to F1..F12 (supports \\r \\n \\t \\xNN;\n"
            "                          repeatable)\n"
            "\n"
            "  -h, --help              show this help\n"
            "  -V, --version           print version and exit\n"
            "\n"
            "runtime (interactive):\n"
            "  Ctrl+A        command menu (x/p/e/c/h/t/b/s/f/r///a/?)\n"
            "  F1..F12       fire bound macros\n"
            "  PgUp/PgDn     scroll back / forward through scrollback\n"
            "  n / N         (in scroll mode, after Ctrl+A /) next / prev match\n"
            "  Up/Down       line history    Left/Right  cursor move\n"
            "  Tab           remote complete (sync-and-flush)\n"
            "  Ctrl+U        clear line      Ctrl+W  delete word\n"
            "  Ctrl+L        clear screen    Ctrl+C  send ETX to remote\n",
            a0, a0);
}

static unsigned parse_baud(const char *s) {
    errno           = 0;
    char         *e = NULL;
    unsigned long v = strtoul(s, &e, 10);
    if (errno || !e || *e != 0 || v == 0 || v > 20000000UL)
        zt_die("zyterm: invalid baud rate: %s", s);
    return (unsigned)v;
}

static void ensure_stdin_fd(void) {
    if (fcntl(STDIN_FILENO, F_GETFD) >= 0 || errno != EBADF) return;

    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0) zt_die("zyterm: open(/dev/null): %s", strerror(errno));
    if (fd != STDIN_FILENO) {
        if (dup2(fd, STDIN_FILENO) < 0) {
            int saved_errno = errno;
            close(fd);
            errno = saved_errno;
            zt_die("zyterm: dup2(/dev/null, stdin): %s", strerror(errno));
        }
        close(fd);
    }
}

int zyterm_main(int argc, char **argv) {
    zt_trace("zyterm_main: enter argc=%d argv0=%s", argc, argv && argv[0] ? argv[0] : "(null)");
    /* reset globals & getopt in case we're re-entered from zy */
    zt_g_quit  = 0;
    zt_g_winch = 0;
#if defined(__GLIBC__)
    optind = 0;
#else
    optind = 1;
#if defined(optreset)
    optreset = 1;
#endif
#endif

    /* Embedded/noninteractive callers can arrive with fd 0 already closed.
     * If we open the serial device first in that state, it steals descriptor 0
     * and zyterm loses its separate stdin/EOF source. Binding stdin to
     * /dev/null preserves the invariant that serial and input stay distinct. */
    ensure_stdin_fd();

    zt_ctx c;
    memset(&c, 0, sizeof c);
    c.serial.fd            = -1;
    c.log.fd               = -1;
    c.net.http_fd          = -1;
    c.serial.epoll_fd      = -1;
    c.ext.filter_stdin_fd  = -1;
    c.ext.filter_stdout_fd = -1;
    c.net.metrics_fd       = -1;
    c.net.session_fd       = -1;
    c.serial.baud          = 115200;
    c.serial.data_bits     = 8;
    c.serial.parity        = 'n';
    c.serial.stop_bits     = 1;
    c.serial.flow          = 0;
    c.core.reconnect       = true; /* auto-reconnect on hang-up by default */
    c.core.replay_speed    = 1.0;
    c.log.line_start       = true;
    c.proto.tx_line_start  = true;
    c.tui.mouse_tracking   = true; /* on → zyterm captures mouse so the wheel
                                    * scrolls scrollback AND click-drag does
                                    * an in-app text selection (copied to
                                    * the system clipboard via OSC 52 on
                                    * release). Toggle OFF with Ctrl+A m
                                    * to fall back to the host terminal's
                                    * native selection (no app-level wheel). */
    c.proto.color_on      = true;
    c.proto.osc52_enabled = true; /* on \u2192 in-app selection copies straight to
                                   * the system clipboard via OSC 52 on every
                                   * left-button release (or right-click on an
                                   * existing selection). Disable with --no-osc52
                                   * if your terminal misrenders the escape. */
    c.tui.hist_view = 0;
    c.log.sb_lines  = calloc(ZT_SCROLLBACK_CAP, sizeof(char *));
    if (!c.log.sb_lines) zt_die("zyterm: out of memory (scrollback)");

    enum {
        OPT_NOCOLOR = 0x1000,
        OPT_TS,
        OPT_DUMP,
        OPT_DATA,
        OPT_PARITY,
        OPT_STOP,
        OPT_FLOW,
        OPT_RECONNECT,
        OPT_NO_RECONNECT,
        OPT_WATCH,
        OPT_WATCH_BEEP,
        OPT_LOG_MAX,
        OPT_TX_TS,
        OPT_REPLAY,
        OPT_REPLAY_SPEED,
        OPT_MACRO,
        OPT_FRAME,
        OPT_CRC,
        OPT_LOG_FORMAT,
        OPT_FILTER,
        OPT_METRICS,
        OPT_HTTP,
        OPT_WEBROOT,
        OPT_HTTP_CORS,
        OPT_DETACH,
        OPT_ATTACH,
        OPT_PROFILE,
        OPT_PROFILE_SAVE,
        OPT_OSC52,
        OPT_NO_OSC52,
        OPT_MUTE_DBG,
        OPT_MUTE_INF,
        OPT_THREADED,
        OPT_EPOLL,
        OPT_AUTOBAUD,
        OPT_DIFF,
    };
    static const struct option lo[] = {
        {"baud", required_argument, NULL, 'b'},
        {"log", required_argument, NULL, 'l'},
        {"hex", no_argument, NULL, 'x'},
        {"echo", no_argument, NULL, 'e'},
        {"no-color", no_argument, NULL, OPT_NOCOLOR},
        {"ts", no_argument, NULL, OPT_TS},
        {"dump", required_argument, NULL, OPT_DUMP},
        {"data", required_argument, NULL, OPT_DATA},
        {"parity", required_argument, NULL, OPT_PARITY},
        {"stop", required_argument, NULL, OPT_STOP},
        {"flow", required_argument, NULL, OPT_FLOW},
        {"reconnect", no_argument, NULL, OPT_RECONNECT},
        {"no-reconnect", no_argument, NULL, OPT_NO_RECONNECT},
        {"watch", required_argument, NULL, OPT_WATCH},
        {"watch-beep", no_argument, NULL, OPT_WATCH_BEEP},
        {"log-max-kb", required_argument, NULL, OPT_LOG_MAX},
        {"tx-ts", no_argument, NULL, OPT_TX_TS},
        {"replay", required_argument, NULL, OPT_REPLAY},
        {"replay-speed", required_argument, NULL, OPT_REPLAY_SPEED},
        {"macro", required_argument, NULL, OPT_MACRO},
        {"frame", required_argument, NULL, OPT_FRAME},
        {"crc", required_argument, NULL, OPT_CRC},
        {"log-format", required_argument, NULL, OPT_LOG_FORMAT},
        {"filter", required_argument, NULL, OPT_FILTER},
        {"metrics", required_argument, NULL, OPT_METRICS},
        {"http", required_argument, NULL, OPT_HTTP},
        {"webroot", required_argument, NULL, OPT_WEBROOT},
        {"http-cors", no_argument, NULL, OPT_HTTP_CORS},
        {"detach", required_argument, NULL, OPT_DETACH},
        {"attach", required_argument, NULL, OPT_ATTACH},
        {"profile", required_argument, NULL, OPT_PROFILE},
        {"profile-save", required_argument, NULL, OPT_PROFILE_SAVE},
        {"osc52", no_argument, NULL, OPT_OSC52},
        {"no-osc52", no_argument, NULL, OPT_NO_OSC52},
        {"mute-dbg", no_argument, NULL, OPT_MUTE_DBG},
        {"mute-inf", no_argument, NULL, OPT_MUTE_INF},
        {"threaded", no_argument, NULL, OPT_THREADED},
        {"epoll", no_argument, NULL, OPT_EPOLL},
        {"autobaud", no_argument, NULL, OPT_AUTOBAUD},
        {"diff", required_argument, NULL, OPT_DIFF},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {0, 0, 0, 0},
    };

    const char *log_path  = NULL;
    int         dump_secs = -1;
    int         opt;
    while ((opt = getopt_long(argc, argv, "b:l:xehV", lo, NULL)) != -1) {
        switch (opt) {
        case 'b': c.serial.baud = parse_baud(optarg); break;
        case 'l': log_path = optarg; break;
        case 'x': c.log.hex_mode = true; break;
        case 'e': c.proto.local_echo = true; break;
        case OPT_NOCOLOR: c.proto.color_on = false; break;
        case OPT_TS: c.proto.show_ts = true; break;
        case OPT_DUMP:
            dump_secs = atoi(optarg);
            if (dump_secs < 0) dump_secs = 0;
            break;
        case OPT_DATA: {
            int v = atoi(optarg);
            if (v < 5 || v > 8) zt_die("zyterm: --data must be 5..8");
            c.serial.data_bits = v;
            break;
        }
        case OPT_PARITY: {
            char p = optarg[0] ? (char)tolower((unsigned char)optarg[0]) : 'n';
            if (p != 'n' && p != 'e' && p != 'o') zt_die("zyterm: --parity must be n|e|o");
            c.serial.parity = p;
            break;
        }
        case OPT_STOP: {
            int v = atoi(optarg);
            if (v != 1 && v != 2) zt_die("zyterm: --stop must be 1 or 2");
            c.serial.stop_bits = v;
            break;
        }
        case OPT_FLOW: {
            char f = optarg[0] ? (char)tolower((unsigned char)optarg[0]) : 'n';
            if (f == 'n' || f == '0')
                c.serial.flow = 0;
            else if (f == 'r' || f == 'c')
                c.serial.flow = 1; /* rts/cts */
            else if (f == 'x' || f == 's')
                c.serial.flow = 2; /* xon/xoff */
            else
                zt_die("zyterm: --flow must be n|r|x");
            break;
        }
        case OPT_RECONNECT: c.core.reconnect = true; break;
        case OPT_NO_RECONNECT: c.core.reconnect = false; break;
        case OPT_WATCH: {
            if (c.log.watch_count >= ZT_WATCH_MAX)
                zt_die("zyterm: too many --watch (max %d)", ZT_WATCH_MAX);
            c.log.watch[c.log.watch_count++] = strdup(optarg);
            break;
        }
        case OPT_WATCH_BEEP: c.log.watch_beep = true; break;
        case OPT_LOG_MAX: {
            long kb = atol(optarg);
            if (kb <= 0) zt_die("zyterm: --log-max-kb must be > 0");
            c.log.max_bytes = (uint64_t)kb * 1024ULL;
            break;
        }
        case OPT_TX_TS: c.proto.tx_ts = true; break;
        case OPT_REPLAY: c.core.replay_path = optarg; break;
        case OPT_REPLAY_SPEED: {
            c.core.replay_speed = atof(optarg);
            if (c.core.replay_speed < 0) c.core.replay_speed = 0;
            break;
        }
        case OPT_MACRO: {
            /* format: F<n>=<string>, n in 1..12 */
            const char *eq = strchr(optarg, '=');
            if (!eq || (optarg[0] != 'F' && optarg[0] != 'f'))
                zt_die("zyterm: --macro format is F<n>=<string>");
            int n = atoi(optarg + 1);
            if (n < 1 || n > ZT_MACRO_COUNT)
                zt_die("zyterm: --macro F<n>: n must be 1..%d", ZT_MACRO_COUNT);
            free(c.ext.macros[n - 1]);
            c.ext.macros[n - 1] = strdup(eq + 1);
            break;
        }
        case OPT_FRAME: {
            if (!strcmp(optarg, "raw"))
                c.proto.mode = ZT_FRAME_RAW;
            else if (!strcmp(optarg, "cobs"))
                c.proto.mode = ZT_FRAME_COBS;
            else if (!strcmp(optarg, "slip"))
                c.proto.mode = ZT_FRAME_SLIP;
            else if (!strcmp(optarg, "hdlc"))
                c.proto.mode = ZT_FRAME_HDLC;
            else if (!strcmp(optarg, "lenpfx"))
                c.proto.mode = ZT_FRAME_LENPFX;
            else
                zt_die("zyterm: --frame must be raw|cobs|slip|hdlc|lenpfx");
            break;
        }
        case OPT_CRC: {
            if (!strcmp(optarg, "none"))
                c.proto.crc_mode = ZT_CRC_NONE;
            else if (!strcmp(optarg, "ccitt"))
                c.proto.crc_mode = ZT_CRC_CCITT;
            else if (!strcmp(optarg, "ibm"))
                c.proto.crc_mode = ZT_CRC_IBM;
            else if (!strcmp(optarg, "crc32"))
                c.proto.crc_mode = ZT_CRC_CRC32;
            else
                zt_die("zyterm: --crc must be none|ccitt|ibm|crc32");
            break;
        }
        case OPT_LOG_FORMAT: {
            if (!strcmp(optarg, "text"))
                c.log.format = ZT_LOG_TEXT;
            else if (!strcmp(optarg, "json"))
                c.log.format = ZT_LOG_JSON;
            else if (!strcmp(optarg, "raw"))
                c.log.format = ZT_LOG_RAW;
            else
                zt_die("zyterm: --log-format must be text|json|raw");
            break;
        }
        case OPT_FILTER:
            free(c.ext.filter_cmd);
            c.ext.filter_cmd = strdup(optarg);
            break;
        case OPT_METRICS:
            free(c.net.metrics_path);
            c.net.metrics_path = strdup(optarg);
            break;
        case OPT_HTTP: c.net.http_port = atoi(optarg); break;
        case OPT_WEBROOT:
            free(c.net.http_webroot);
            c.net.http_webroot = strdup(optarg);
            break;
        case OPT_HTTP_CORS: c.net.http_cors = true; break;
        case OPT_DETACH:
            free(c.net.session_name);
            c.net.session_name = strdup(optarg);
            break;
        case OPT_ATTACH: {
            int rc = session_attach(optarg);
            scrollback_free(&c);
            return rc;
        }
        case OPT_PROFILE: profile_load(&c, optarg); break;
        case OPT_PROFILE_SAVE:
            free(c.net.session_name);
            c.net.session_name = strdup(optarg);
            /* save at shutdown; we stash the name in session_name */
            break;
        case OPT_OSC52: c.proto.osc52_enabled = true; break;
        case OPT_NO_OSC52: c.proto.osc52_enabled = false; break;
        case OPT_MUTE_DBG: c.log.mute_dbg = true; break;
        case OPT_MUTE_INF: c.log.mute_inf = true; break;
        case OPT_THREADED: c.serial.spsc_enabled = true; break;
        case OPT_EPOLL: /* fastio_init deferred until c is populated */ break;
        case OPT_AUTOBAUD:
            c.serial.baud = 0; /* sentinel: probe after open fails */
            break;
        case OPT_DIFF: {
            const char *a = optarg;
            if (optind >= argc) zt_die("zyterm: --diff needs two file args");
            const char *b  = argv[optind++];
            int         rc = diff_run(a, b);
            scrollback_free(&c);
            return rc;
        }
        case 'V':
            printf("zyterm " ZT_VERSION "\n");
            scrollback_free(&c);
            return 0;
        case 'h':
            usage(argv[0]);
            scrollback_free(&c);
            return 0;
        default:
            usage(argv[0]);
            scrollback_free(&c);
            return 2;
        }
    }

    /* replay mode: no device needed */
    if (c.core.replay_path) {
        c.serial.device = c.core.replay_path;
        if (log_path) {
            c.log.path = log_path;
            c.log.fd   = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
            if (c.log.fd < 0) zt_die("zyterm: open(%s): %s", log_path, strerror(errno));
        }
        install_signals();
        int rc = run_replay(&c);
        if (c.log.fd >= 0) close(c.log.fd);
        scrollback_free(&c);
        history_free(&c);
        for (int i = 0; i < c.log.watch_count; i++)
            free(c.log.watch[i]);
        for (int i = 0; i < ZT_MACRO_COUNT; i++)
            free(c.ext.macros[i]);
        return rc;
    }

    if (optind >= argc) {
        usage(argv[0]);
        scrollback_free(&c);
        return 2;
    }
    c.serial.device = argv[optind];

    if (log_path) {
        c.log.path = log_path;
        c.log.fd   = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (c.log.fd < 0) zt_die("zyterm: open(%s): %s", log_path, strerror(errno));
        struct stat st;
        if (fstat(c.log.fd, &st) == 0) c.log.bytes = (uint64_t)st.st_size;
    }

    install_signals();
    c.serial.fd = setup_serial(c.serial.device, c.serial.baud, c.serial.data_bits,
                               c.serial.parity, c.serial.stop_bits, c.serial.flow);

    /* ── start subsystems that were configured via CLI flags ── */
    if (c.net.http_port > 0) http_start(&c, c.net.http_port);
    if (c.net.metrics_path) metrics_start(&c, c.net.metrics_path);
    if (c.ext.filter_cmd) filter_start(&c, c.ext.filter_cmd);
    if (c.net.session_name) session_detach(&c, c.net.session_name);
    if (c.serial.spsc_enabled) rx_thread_start(&c);

    int rc = (dump_secs >= 0) ? run_dump(&c, dump_secs) : run_interactive(&c);

    /* ── Restore terminal FIRST — before any cleanup that could crash.
     * The atexit(restore_terminal) registered inside run_interactive /
     * run_replay is a safety net, but it fires only after ALL other
     * cleanup.  If scrollback_free / history_free or a subsystem stop
     * faults, the atexit handler never runs and the parent shell is
     * left in raw-mode + alt-screen (appears "stuck"). */
    ob_flush();
    restore_terminal();

    /* Standalone UX: hand control back on a fresh line so the parent shell
     * prompt does not get rendered adjacent to stale command text. For the
     * embedded builtin path, leave prompt ownership to the host shell. */
    if (!zt_g_embedded) (void)zt_write_cstr(STDOUT_FILENO, "\r\n");

    /* Safety net: if any cleanup step below hangs (e.g. close() on a
     * disconnected USB-serial device), SIGALRM will terminate us after
     * 3 seconds.  The terminal is already restored at this point.
     *
     * When embedded inside zy, the default SIGALRM action (terminate)
     * would also kill the host shell — so we only arm the watchdog
     * when running as a standalone process. */
    if (!zt_g_embedded) alarm(3);

    /* Close serial fd early — this unblocks the rx thread (if active)
     * so pthread_join in rx_thread_stop returns promptly. */
    if (c.serial.fd >= 0) {
        close(c.serial.fd);
        c.serial.fd = -1;
    }

    /* ── stop subsystems ── */
    rx_thread_stop(&c);
    http_stop(&c);
    metrics_stop(&c);
    filter_stop(&c);

    if (c.log.fd >= 0) close(c.log.fd);
    scrollback_free(&c);
    history_free(&c);
    for (int i = 0; i < c.log.watch_count; i++)
        free(c.log.watch[i]);
    for (int i = 0; i < ZT_MACRO_COUNT; i++)
        free(c.ext.macros[i]);
    for (int i = 0; i < c.log.bookmark_count; i++)
        free(c.log.bookmark_notes[i]);
    free(c.ext.filter_cmd);
    free(c.net.metrics_path);
    free(c.net.session_name);
    free(c.net.http_webroot);
    return rc;
}

#ifndef ZYTERM_NO_MAIN
int main(int argc, char **argv) {
    return zyterm_main(argc, argv);
}
#endif
