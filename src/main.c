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

/* Modern, color-aware help layout. Auto-disables ANSI when stderr is not a
 * TTY (piping to less/grep/file) or when NO_COLOR is set. Help goes to
 * stderr by historical convention so `zyterm 2>/dev/null` still behaves. */

/* Visible-width column where descriptions start. */
#define HELP_DESC_COL 34

/* Print one option row with deterministic alignment in plain mode. ANSI
 * sequences in @flag / @val are visually zero-width so we measure with
 * @flag_visible_len passed in (caller knows the plain text). */
static void help_row(FILE *fp, bool tty, const char *short_opt,
                     const char *long_opt, const char *val_plain,
                     const char *desc) {
    /* ANSI palette (mirrors usage()). */
    const char *RST  = tty ? "\033[0m"    : "";
    const char *FLAG = tty ? "\033[1;32m" : "";
    const char *VAL  = tty ? "\033[33m"   : "";
    const char *MUT  = tty ? "\033[2;37m" : "";

    /* Build the visible-width prefix:
     *   "  " + (short_opt ? "-X, " : "    ") + long_opt + " " + val
     */
    int vis = 2 + 4 + (int) strlen(long_opt);
    if (val_plain && *val_plain) vis += 1 + (int) strlen(val_plain);

    fputs("  ", fp);
    if (short_opt) fprintf(fp, "%s%s%s, ", FLAG, short_opt, RST);
    else           fputs("    ", fp);
    fprintf(fp, "%s%s%s", FLAG, long_opt, RST);
    if (val_plain && *val_plain) fprintf(fp, " %s%s%s", VAL, val_plain, RST);

    int pad = HELP_DESC_COL - vis;
    if (pad < 1) pad = 1;
    for (int i = 0; i < pad; i++) fputc(' ', fp);

    fprintf(fp, "%s%s%s\n", MUT, desc, RST);
}

/* Continuation line under a row — indented to the description column. */
static void help_cont(FILE *fp, bool tty, const char *desc) {
    const char *RST = tty ? "\033[0m"    : "";
    const char *MUT = tty ? "\033[2;37m" : "";
    for (int i = 0; i < HELP_DESC_COL; i++) fputc(' ', fp);
    fprintf(fp, "%s%s%s\n", MUT, desc, RST);
}

static void usage(const char *a0) {
    bool tty = isatty(STDERR_FILENO) && !getenv("NO_COLOR");
    const char *T = getenv("TERM");
    if (T && !strcmp(T, "dumb")) tty = false;

    const char *RST  = tty ? "\033[0m"    : "";
    const char *DIM  = tty ? "\033[2m"    : "";
    const char *B    = tty ? "\033[1m"    : "";
    const char *HEAD = tty ? "\033[1;36m" : "";
    const char *VAL  = tty ? "\033[33m"   : "";
    const char *URL  = tty ? "\033[35m"   : "";
    const char *MUT  = tty ? "\033[2;37m" : "";
    const char *ACC  = tty ? "\033[1;35m" : "";
    const char *FLAG = tty ? "\033[1;32m" : "";

    FILE *fp = stderr;

    /* Banner */
    fprintf(fp,
            "\n  %s▍%s %szyterm%s %s%s%s  %s· high-performance serial terminal%s\n"
            "  %s▍%s %swww.iskandarputra.com%s · %sMIT%s\n\n",
            ACC, RST, B, RST, DIM, ZT_VERSION, RST, MUT, RST,
            ACC, RST, URL, RST, DIM, RST);

    /* Usage */
    fprintf(fp, "%sUSAGE%s\n", HEAD, RST);
    fprintf(fp, "  %s%s%s [%sOPTIONS%s] %s<DEVICE>%s\n",
            B, a0, RST, FLAG, RST, VAL, RST);
    fprintf(fp, "  %s%s%s --replay %s<LOGFILE>%s\n\n",
            B, a0, RST, VAL, RST);

    /* Device */
    fprintf(fp, "%sDEVICE%s\n", HEAD, RST);
    fprintf(fp, "  %s/dev/ttyUSB0%s         %sserial node (or any character device)%s\n",
            VAL, RST, MUT, RST);
    fprintf(fp, "  %stcp://%s%shost:port%s       %sraw TCP (e.g. ser2net in raw mode)%s\n",
            URL, RST, VAL, RST, MUT, RST);
    fprintf(fp, "  %stelnet://%s%shost:port%s    %sTCP + Telnet IAC escaping%s\n",
            URL, RST, VAL, RST, MUT, RST);
    fprintf(fp, "  %srfc2217://%s%shost:port%s   %sNYI — use ser2net raw + tcp://%s\n\n",
            URL, RST, VAL, RST, MUT, RST);

    /* Connection */
    fprintf(fp, "%sCONNECTION%s\n", HEAD, RST);
    help_row (fp, tty, "-b", "--baud",          "<rate>",      "baud rate (default 115200; arbitrary via termios2)");
    help_row (fp, tty, NULL, "--data",          "<5|6|7|8>",   "data bits (default 8)");
    help_row (fp, tty, NULL, "--parity",        "<n|e|o>",     "parity: none | even | odd (default n)");
    help_row (fp, tty, NULL, "--stop",          "<1|2>",       "stop bits (default 1)");
    help_row (fp, tty, NULL, "--flow",          "<n|r|x>",     "none | rts/cts | xon/xoff (default n)");
    help_row (fp, tty, NULL, "--reconnect",     "",            "auto-reopen on hang-up (default ON)");
    help_row (fp, tty, NULL, "--no-reconnect",  "",            "exit on serial hang-up");
    help_row (fp, tty, NULL, "--port-glob",     "<pat>",       "re-resolve device on each reconnect");
    help_cont(fp, tty,                                         "e.g. \"/dev/ttyUSB*\"; <DEVICE> becomes optional");
    help_row (fp, tty, NULL, "--match-vid-pid", "<V:P>",       "filter discovered ports by USB id (hex)");
    help_cont(fp, tty,                                         "e.g. 0403:6001 (FT232R), 1a86:7523 (CH340)");
    fputc('\n', fp);

    /* Logging */
    fprintf(fp, "%sLOGGING & CAPTURE%s\n", HEAD, RST);
    help_row (fp, tty, "-l", "--log",           "<file>",      "append log with ms timestamps");
    help_cont(fp, tty,                                         "Ctrl+A l toggles auto-named zyterm-YYYYMMDD-NNN.txt");
    help_row (fp, tty, NULL, "--log-max-kb",    "<N>",         "rotate to <file>.1 when it exceeds N KB");
    help_row (fp, tty, NULL, "--tx-ts",         "",            "also log TX with \"-> \" prefix");
    help_row (fp, tty, NULL, "--dump",          "<sec>",       "headless capture for N seconds (0 = forever)");
    help_row (fp, tty, NULL, "--replay",        "<file>",      "replay a capture through the UI");
    help_row (fp, tty, NULL, "--replay-speed",  "<x>",         "speed multiplier (default 1.0, 0 = max)");
    help_row (fp, tty, NULL, "--rec",           "<file>",      "record session as asciinema cast v2");
    help_cont(fp, tty,                                         "play back with: asciinema play <file>");
    fputc('\n', fp);

    /* Display & input */
    fprintf(fp, "%sDISPLAY & INPUT%s\n", HEAD, RST);
    help_row (fp, tty, "-x", "--hex",           "",            "render RX as hex dump");
    help_row (fp, tty, "-e", "--echo",          "",            "start with local echo on");
    help_row (fp, tty, NULL, "--no-color",      "",            "disable RX log-level colouring");
    help_row (fp, tty, NULL, "--ts",            "",            "start with timestamp display on");
    help_row (fp, tty, NULL, "--watch",         "<pattern>",   "highlight matching lines (repeatable, up to 8)");
    help_row (fp, tty, NULL, "--watch-beep",    "",            "BEL on watch match");
    help_row (fp, tty, NULL, "--macro",         "F<n>=<str>",  "bind macro to F1..F12 (\\r \\n \\t \\xNN; repeatable)");
    help_row (fp, tty, NULL, "--map-out",       "<mode>",      "rewrite outgoing line endings");
    help_row (fp, tty, NULL, "--map-in",        "<mode>",      "rewrite incoming line endings");
    help_cont(fp, tty,                                         "mode: none | cr | lf | crlf | cr-crlf | lf-crlf");
    fputc('\n', fp);

    /* Profiles */
    fprintf(fp, "%sPROFILES%s\n", HEAD, RST);
    help_row (fp, tty, NULL, "--profile",       "<name>",      "load ~/.config/zyterm/<name>.conf at startup");
    help_cont(fp, tty,                                         "auto-reloads on edit (Linux inotify)");
    help_row (fp, tty, NULL, "--profile-save",  "<name>",      "snapshot current settings to that profile and exit");
    fputc('\n', fp);

    /* Misc */
    fprintf(fp, "%sHELP%s\n", HEAD, RST);
    help_row (fp, tty, "-h", "--help",          "",            "show this help");
    help_row (fp, tty, "-V", "--version",       "",            "print version and exit");
    fputc('\n', fp);

    /* Interactive */
    fprintf(fp, "%sINTERACTIVE%s\n", HEAD, RST);
    fprintf(fp, "  %sCtrl+A%s        command menu  %s(q x p e c h t b s f r / a ?)%s\n", B, RST, MUT, RST);
    fprintf(fp, "  %sF1..F12%s       fire bound macros\n", B, RST);
    fprintf(fp, "  %sPgUp%s/%sPgDn%s     scroll back / forward through scrollback\n", B, RST, B, RST);
    fprintf(fp, "  %sn%s / %sN%s         %s(in scroll mode after Ctrl+A /)%s next / prev match\n",
            B, RST, B, RST, MUT, RST);
    fprintf(fp, "  %sUp%s/%sDown%s       line history       %sLeft%s/%sRight%s  cursor move\n",
            B, RST, B, RST, B, RST, B, RST);
    fprintf(fp, "  %sTab%s           remote completion (sync-and-flush)\n", B, RST);
    fprintf(fp, "  %sCtrl+U%s        clear line         %sCtrl+W%s  delete word\n", B, RST, B, RST);
    fprintf(fp, "  %sCtrl+L%s        clear screen       %sCtrl+C%s  send ETX to remote\n", B, RST, B, RST);

    fprintf(fp, "\n%sExamples%s\n", HEAD, RST);
    fprintf(fp, "  %s$%s %szyterm /dev/ttyUSB0 -b 115200%s\n",                  DIM, RST, B, RST);
    fprintf(fp, "  %s$%s %szyterm --match-vid-pid 1a86:7523 --map-out crlf%s\n", DIM, RST, B, RST);
    fprintf(fp, "  %s$%s %szyterm tcp://lab-pi.local:23000 -l boot.log%s\n\n",   DIM, RST, B, RST);
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
        OPT_MAP_OUT,
        OPT_MAP_IN,
        OPT_PORT_GLOB,
        OPT_MATCH_VID_PID,
        OPT_REC,
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
        {"map-out", required_argument, NULL, OPT_MAP_OUT},
        {"map-in", required_argument, NULL, OPT_MAP_IN},
        {"port-glob", required_argument, NULL, OPT_PORT_GLOB},
        {"match-vid-pid", required_argument, NULL, OPT_MATCH_VID_PID},
        {"rec", required_argument, NULL, OPT_REC},
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
        case OPT_PROFILE:
            profile_load(&c, optarg);
            c.ext.profile_name = optarg;
            break;
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
        case OPT_MAP_OUT:
            if (eol_parse(optarg, &c.proto.map_out) < 0)
                zt_die("zyterm: --map-out must be none|cr|lf|crlf|cr-crlf|lf-crlf");
            break;
        case OPT_MAP_IN:
            if (eol_parse(optarg, &c.proto.map_in) < 0)
                zt_die("zyterm: --map-in must be none|cr|lf|crlf|cr-crlf|lf-crlf");
            break;
        case OPT_PORT_GLOB:
            c.serial.port_glob = optarg;
            break;
        case OPT_MATCH_VID_PID: {
            /* format: VVVV:PPPP, hex (e.g. 0403:6001 = FT232R) */
            char *colon = strchr(optarg, ':');
            if (!colon)
                zt_die("zyterm: --match-vid-pid must be VVVV:PPPP (hex)");
            char *end = NULL;
            unsigned long v = strtoul(optarg, &end, 16);
            if (end != colon || v > 0xFFFFu)
                zt_die("zyterm: --match-vid-pid: bad vendor id");
            unsigned long p = strtoul(colon + 1, &end, 16);
            if (*end != '\0' || p > 0xFFFFu)
                zt_die("zyterm: --match-vid-pid: bad product id");
            c.serial.match_vid = (uint16_t) v;
            c.serial.match_pid = (uint16_t) p;
            break;
        }
        case OPT_REC:
            c.log.rec_path = optarg;
            break;
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
        /* No positional device — accept it only when discovery hints are
         * present, in which case we resolve a real path before opening. */
        if (!c.serial.port_glob && !c.serial.match_vid && !c.serial.match_pid) {
            usage(argv[0]);
            scrollback_free(&c);
            return 2;
        }
        char *found = port_discover(c.serial.port_glob,
                                    c.serial.match_vid, c.serial.match_pid);
        if (!found)
            zt_die("zyterm: no device matched --port-glob / --match-vid-pid");
        c.serial.device = found;
    } else {
        c.serial.device = argv[optind];
    }

    if (log_path) {
        c.log.path = log_path;
        c.log.fd   = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (c.log.fd < 0) zt_die("zyterm: open(%s): %s", log_path, strerror(errno));
        struct stat st;
        if (fstat(c.log.fd, &st) == 0) c.log.bytes = (uint64_t)st.st_size;
    }

    install_signals();
    c.serial.is_socket = transport_is_url(c.serial.device);
    c.serial.telnet    = c.serial.is_socket
                         && strncmp(c.serial.device, "telnet://", 9) == 0;
    c.serial.fd = setup_serial(c.serial.device, c.serial.baud, c.serial.data_bits,
                               c.serial.parity, c.serial.stop_bits, c.serial.flow);

    /* ── start subsystems that were configured via CLI flags ── */
    if (c.net.http_port > 0) http_start(&c, c.net.http_port);
    if (c.net.metrics_path) metrics_start(&c, c.net.metrics_path);
    if (c.ext.filter_cmd) filter_start(&c, c.ext.filter_cmd);
    if (c.net.session_name) session_detach(&c, c.net.session_name);
    if (c.serial.spsc_enabled) rx_thread_start(&c);

    if (c.ext.profile_name) (void) profile_watch_start(&c, c.ext.profile_name);

    if (c.log.rec_path) {
        if (cast_record_open(&c, c.log.rec_path) != 0)
            zt_die("zyterm: --rec %s: %s", c.log.rec_path, strerror(errno));
    }

    int rc = (dump_secs >= 0) ? run_dump(&c, dump_secs) : run_interactive(&c);

    /* ── Restore terminal FIRST — before any cleanup that could crash.
     * The atexit(restore_terminal) registered inside run_interactive /
     * run_replay is a safety net, but it fires only after ALL other
     * cleanup.  If scrollback_free / history_free or a subsystem stop
     * faults, the atexit handler never runs and the parent shell is
     * left in raw-mode + alt-screen (appears "stuck"). */
    ob_flush();
    cast_record_close(&c);
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
    profile_watch_stop(&c);

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
