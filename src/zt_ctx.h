/**
 * @file    zt_ctx.h
 * @brief   Compile-time tunables and the shared @ref zt_ctx context struct.
 *
 * Every module in zyterm operates on a single @ref zt_ctx instance that
 * carries all runtime state: open file descriptors, input buffers,
 * rendering flags, scrollback ring, and mode toggles. Keeping this in one
 * header avoids circular `#include`s between modules and lets the compiler
 * inline member accesses aggressively.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#ifndef ZYTERM_ZT_CTX_H
#define ZYTERM_ZT_CTX_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <termios.h>
#include <time.h>

#if defined(__linux__)
/** @brief Linux extension for arbitrary baud rates via @c BOTHER.
 *  Re-declared here because `<asm/termbits.h>` conflicts with `<termios.h>`. */
struct termios2 {
    tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
    cc_t     c_line;
    cc_t     c_cc[19];
    speed_t  c_ispeed, c_ospeed;
};
#ifndef TCGETS2
#define TCGETS2 _IOR('T', 0x2A, struct termios2)
#endif
#ifndef TCSETS2
#define TCSETS2 _IOW('T', 0x2B, struct termios2)
#endif
#ifndef BOTHER
#define BOTHER 0010000
#endif
#ifndef CBAUD
#define CBAUD 0010017
#endif
#elif defined(__APPLE__)
#include <IOKit/serial/ioss.h>
#endif

/** @name Compile-time limits @{ */
#define ZT_INPUT_CAP           4096       /**< Max chars in the on-screen input line.   */
#define ZT_LINEBUF_CAP         4096       /**< Max chars in a single log/render line.   */
#define ZT_READ_CHUNK          65536      /**< Largest single read() from serial/stdin. */
#define ZT_HISTORY_CAP         128        /**< Command-history ring capacity.           */
#define ZT_FLUSH_DELAY_US      2000       /**< Stdout output buffer flush delay.        */
#define ZT_HUD_REFRESH_MS      500        /**< HUD repaint cadence.                     */
#define ZT_SCROLLBACK_CAP      10000      /**< Scrollback ring-buffer line count.       */
#define ZT_WATCH_MAX           8          /**< Max @c --watch patterns.                 */
#define ZT_MACRO_COUNT         12         /**< F1..F12.                                 */
#define ZT_SEARCH_CAP          128        /**< Max chars in a search query.             */
#define ZT_RECONNECT_MS        1000       /**< Reconnect-loop poll interval.            */
#define PATH_MAX_LEN           1024       /**< Max length for log path / device path.   */
#define ZT_BOOKMARK_MAX        64         /**< Max user-set scrollback bookmarks.       */
#define ZT_SPARK_HIST          32         /**< Throughput sparkline sample depth.       */
#define ZT_SPSC_CAP            (1u << 20) /**< Reader-thread SPSC ring (1 MiB).         */
#define ZT_VERSION             "1.3.0"
#define ZT_SGR_PARAM_CAP       64   /**< Max CSI param/intermediate bytes (SGR filter).  */
#define ZT_DEVLINE_CAP         1024 /**< Max bytes modeled on the device's current line. */
#define ZT_DEVLINE_CSI_CAP     32   /**< Max CSI param/intermediate bytes (devline).     */
#define ZT_RECONCILE_TAIL_MAX  256  /**< Max chars a completion may append to input.     */
#define ZT_RECONCILE_WINDOW_MS 1500 /**< Post-Tab reconcile window (cancelled on any edit). */
/** @} */

/** @name Device-RX SGR-only filter (ADR-0009) @{ */
/** Bounded CSI parser state for the device-RX SGR filter. Lives in
 *  @ref zt_ctx::proto so a sequence split across read() chunks resumes
 *  correctly. NONE is the zero-init default. See INVARIANTS §6. */
typedef enum { ZT_SGR_NONE = 0, ZT_SGR_ESC, ZT_SGR_CSI } zt_sgr_state;

typedef struct {
    zt_sgr_state  state;                 /**< NONE / ESC seen / in CSI.        */
    unsigned char buf[ZT_SGR_PARAM_CAP]; /**< CSI params+intermediates only.   */
    size_t        len;                   /**< Bytes used in @c buf.            */
} zt_sgr_parser;

/** Disposition @ref sgr_feed returns for one device byte. */
typedef enum {
    ZT_SGR_ACT_HOLD,     /**< Byte consumed mid-sequence; render nothing.      */
    ZT_SGR_ACT_EMIT_SGR, /**< A complete allowed SGR is in out[]; emit it.     */
    ZT_SGR_ACT_INERT,    /**< Render out[] as inert caret-notation text.       */
    ZT_SGR_ACT_REPROCESS /**< Render out[] inert, then re-feed the byte.       */
} zt_sgr_act;
/** @} */

/** @name Device prompt-line reconciliation (ADR-0010) @{ */
/** A bounded model of the characters currently on the DEVICE's prompt line,
 *  rebuilt from RX by interpreting a minimal line discipline (printable, CR,
 *  BS, LF, and ESC[K / ESC[nC / ESC[nD). Reset on '\n', so async log lines
 *  (separate, newline-terminated) never pollute it. Used to mirror a device
 *  Tab-completion into the local input line. Cursor (@c col) is a byte offset;
 *  the adopted completion tail is restricted to a single token (word chars), so
 *  it stops at the space/'[' that begins an inline log line, and a non-ASCII
 *  completion degrades to no-adopt rather than corrupting input. See
 *  INVARIANTS §6 / ADR-0010. */
typedef struct {
    enum { ZT_DL_NONE = 0, ZT_DL_ESC, ZT_DL_CSI, ZT_DL_OSC } st; /**< Escape consumer. */
    unsigned char buf[ZT_DEVLINE_CAP];     /**< Visible bytes of the current line. */
    size_t        len;                     /**< High-water extent written in buf.  */
    size_t        col;                     /**< Cursor column (byte offset).       */
    bool          overflowed;              /**< Exceeded CAP → suppress adoption.  */
    unsigned char csi[ZT_DEVLINE_CSI_CAP]; /**< Accumulated CSI params.            */
    size_t        csi_len;
} zt_devline;
/** @} */

/** @brief Logical frame modes for the @ref framing module.
 *
 * When not @c ZT_FRAME_RAW, the framing decoder groups bytes into frames and
 * renders one scrollback entry per frame (with optional CRC check). */
typedef enum {
    ZT_FRAME_RAW    = 0, /**< No framing — line-based raw text mode (default). */
    ZT_FRAME_COBS   = 1, /**< Consistent-Overhead Byte Stuffing (RFC-esque).   */
    ZT_FRAME_SLIP   = 2, /**< RFC 1055 SLIP (0xC0 terminator + escape).        */
    ZT_FRAME_HDLC   = 3, /**< HDLC-ish (0x7E flag + 0x7D escape, no bit stuf). */
    ZT_FRAME_LENPFX = 4, /**< <len16 LE><payload> length-prefixed frames.      */
    ZT_FRAME__COUNT
} zt_frame_mode;

/** @brief CRC algorithms available via @c --crc / @c Ctrl+A C. */
typedef enum {
    ZT_CRC_NONE  = 0,
    ZT_CRC_CCITT = 1, /**< CRC-16-CCITT-FALSE (poly 0x1021, init 0xFFFF).      */
    ZT_CRC_IBM   = 2, /**< CRC-16-IBM / Modbus (poly 0xA001 reflected).        */
    ZT_CRC_CRC32 = 3, /**< CRC-32 (poly 0xEDB88320 reflected, a.k.a. PKZIP).   */
    ZT_CRC__COUNT
} zt_crc_mode;

/** @brief Log file format. */
typedef enum {
    ZT_LOG_TEXT = 0, /**< Line-prefixed human-readable (default).                */
    ZT_LOG_JSON = 1, /**< NDJSON/JSONL stream: one JSON object per line.         */
    ZT_LOG_RAW  = 2, /**< Raw byte capture (splice(2) fast path when possible). */
    ZT_LOG__COUNT
} zt_log_format;

/** @brief Line-ending translation modes for `--map-out` / `--map-in`.
 *
 * Some firmware expects bare CR, others bare LF, others CRLF. Rather
 * than force the user to retype with `Ctrl+V <CR>` tricks, zyterm can
 * translate end-of-line bytes on the wire. Mappings are direction-
 * independent: the same enum value picks both an outgoing and an
 * incoming rewrite, applied symmetrically.
 *
 * | mode             | outgoing (typed → device) | incoming (device → screen) |
 * |------------------|---------------------------|----------------------------|
 * | NONE             | passthrough                | passthrough                |
 * | CR               | LF → CR                    | CR → LF                    |
 * | LF               | CR → LF                    | LF → CR                    |
 * | CRLF             | LF → CRLF, lone CR → CRLF  | CRLF → LF                  |
 * | CR_CRLF          | CR → CRLF                  | CRLF → CR                  |
 * | LF_CRLF          | LF → CRLF                  | CRLF → LF                  |
 *
 * State is per-direction (a one-byte "saw_cr" latch) so multi-byte
 * sequences split across read() boundaries still round-trip correctly. */
typedef enum {
    ZT_EOL_NONE    = 0,
    ZT_EOL_CR      = 1,
    ZT_EOL_LF      = 2,
    ZT_EOL_CRLF    = 3,
    ZT_EOL_CR_CRLF = 4,
    ZT_EOL_LF_CRLF = 5,
    ZT_EOL__COUNT
} zt_eol_map;

/** @brief Per-direction state for the line-ending translator.
 *  @c saw_cr is set when the previous input byte was a CR and the
 *  active mode coalesces CRLF pairs. */
typedef struct {
    uint8_t saw_cr;
} zt_eol_state;

/* ---------------------------- global (signal-safe) state ------------------ */
extern volatile sig_atomic_t zt_g_quit;  /**< Set by SIGINT/TERM/HUP/QUIT. */
extern volatile sig_atomic_t zt_g_winch; /**< Set by SIGWINCH.             */

extern struct termios        zt_g_orig_stdin;
extern int                   zt_g_orig_stdin_fl;
extern bool                  zt_g_stdin_saved;
extern bool                  zt_g_ui_active;
/** Non-zero when zyterm is running embedded inside the zy shell.
 *  Used to suppress process-terminating behaviour (alarm, _exit)
 *  that would also kill the host shell. */
extern bool zt_g_embedded;

/* Embed bail-out hook \u2014 see the matching definitions in core.c.
 * The host arms the jump buffer with sigsetjmp() before calling
 * zyterm_main(); fatal paths siglongjmp() back to it so the host
 * shell survives. Disarm with zt_embed_disarm() on a normal return. */
#include <setjmp.h>
extern sigjmp_buf zt_g_embed_jmp;
extern bool       zt_g_embed_jmp_armed;
void              zt_embed_disarm(void);

/**
 * @struct zt_ctx
 * @brief  Master runtime context passed to every subsystem.
 *
 * All fields are owned by the process; most are mutated only by the main
 * thread (zyterm uses a high-priority reader thread for serial I/O).
 * Signal handlers only touch the `g_*` globals above.
 */
typedef struct {
    struct {
        const char *device; /**< Serial device path, e.g. `/dev/ttyUSB0`. */
        unsigned    baud;   /**< Baud rate in bps.                        */
        int         fd;     /**< Serial FD, or -1 when disconnected.      */

        int         data_bits; /**< 5..8.                                    */
        char        parity;    /**< 'n' | 'e' | 'o'.                         */
        int         stop_bits; /**< 1 | 2.                                   */
        int         flow;      /**< 0=none, 1=rtscts, 2=xonxoff.             */
        bool        reconnect;

        /* Tier 1 — port discovery (USB hot-plug, --port-glob, --match-vid-pid).
         * When set, reconnect_attempt() will re-resolve the device path on every
         * retry so a re-enumerated USB-serial adapter (USB0 → USB1 after replug)
         * is found again. The originally-selected device path is held in
         * @c device; the discovery hints survive across reconnects. */
        const char *port_glob; /**< e.g. "/dev/ttyUSB*", NULL = off.         */
        uint16_t    match_vid; /**< USB vendor id (0 = any / off).           */
        uint16_t    match_pid; /**< USB product id (0 = any / off).          */

        /* Tier 1 — TCP / telnet / RFC 2217 transport (--device tcp://host:port).
         * When @c is_socket is true, the fd is a connected stream socket and
         * tty-only ioctls (TIOCGICOUNT, TIOCMGET, tcsendbreak, custom baud)
         * silently no-op. @c telnet enables IAC escaping/stripping. */
        bool    is_socket;
        bool    telnet;       /**< TX 0xFF→0xFF 0xFF; strip RX IAC seq.    */
        uint8_t telnet_rx_st; /**< Telnet RX parser state (see transport.c).*/

        /* Tier 1 — line-state & kernel counters */
        unsigned        kern_frame_err;   /**< TIOCGICOUNT frame.                       */
        unsigned        kern_overrun_err; /**< TIOCGICOUNT overrun.                     */
        unsigned        kern_parity_err;  /**< TIOCGICOUNT parity.                      */
        unsigned        kern_brk;         /**< TIOCGICOUNT brk.                         */
        unsigned        kern_buf_overrun; /**< TIOCGICOUNT buf_overrun.                 */
        unsigned        modem_lines;      /**< TIOCMGET mask (DCD/DTR/DSR/CTS/RI/RTS).  */
        struct timespec t_last_stats;     /**< Last TIOCGICOUNT poll time.              */

        /* Tier 1 — SPSC reader thread */
        bool  spsc_enabled;      /**< Reader thread is on.                     */
        int   spsc_wake_pipe[2]; /**< Main wakes via eventfd-ish pipe.         */
        void *spsc_impl;         /**< Opaque — defined in rx_thread.c.         */

        /* Tier 1 — epoll fd (Linux fast path) */
        int epoll_fd; /**< Linux only; -1 otherwise.                */
    } serial;

    struct {
        int           rows, cols;              /**< Current terminal size.                   */
        unsigned char input_buf[ZT_INPUT_CAP]; /**< Editable input line bytes.      */
        size_t        input_len;               /**< Valid length in @ref input_buf.          */
        size_t        sent_len;                /**< Prefix already committed to the device.  */
        size_t        cursor;                  /**< Offset from @c sent_len into unsent.     */

        /* Tab-completion reconciliation (ADR-0010): armed by Tab, cleared by any
         * edit or the deadline. While armed, a device prompt-line completion may
         * EXTEND (never replace) input_buf. */
        bool            reconcile_pending;
        size_t          reconcile_cmd_len; /**< input_len snapshot at Tab.           */
        struct timespec reconcile_armed;   /**< Monotonic time Tab armed the window. */

        char           *hist[ZT_HISTORY_CAP];
        int             hist_count;
        int             hist_head;
        int             hist_view; /**< 0 = live edit, else back-step.           */

        bool            command_mode;
        bool            ui_dirty;
        char            flash[96];
        struct timespec flash_until;

        bool            popup_active; /**< Command popup or dialog on-screen.       */
        bool            disconnected; /**< Reconnect popup is owning the screen —
                                            handle_cmd_key gates side-effecting
                                            keys behind this. */
        bool never_connected;         /**< Cold start: the device was absent
                                           at launch and we have not opened a
                                           link yet. Flips the reconnect popup
                                           + HUD wording from "interrupted /
                                           reconnected" to "waiting / connected"
                                           and suppresses the spurious DISCONNECT
                                           hook on boot. Cleared on first open. */
        bool keybind_visible;         /**< Ctrl+A k/? help popup on screen;
                                           ANY next key dismisses it via the
                                           transient-overlay check at the top
                                           of handle_stdin_chunk. */

        /* Tier 4 — settings menu (minicom-style) */
        bool settings_mode; /**< Settings dialog is on-screen.            */
        int  settings_page; /**< 0=serial, 1=screen, 2=kbd, 3=logging.   */

        /* Tier 4 — pager */
        bool pager_mode; /**< less-style navigation over scrollback.   */

        /* scrollback search */
        bool   search_mode;
        char   search_buf[ZT_SEARCH_CAP];
        size_t search_len;
        int    search_hits;
        int    search_current;

        /* Tier 4 — fuzzy finder overlay */
        bool   fuzzy_mode;
        char   fuzzy_buf[ZT_SEARCH_CAP];
        size_t fuzzy_len;
        int    fuzzy_selected;

        /* log rename mini-input */
        bool   rename_mode;
        char   rename_buf[PATH_MAX_LEN];
        size_t rename_len;

        /* mouse */
        bool mouse_tracking; /**< Drag-motion (?1002h) toggle.             */
        bool sb_dragging;

        /* in-app text selection */
        bool sel_active;      /**< A selection currently exists.            */
        bool sel_dragging;    /**< Left button held + dragging.             */
        int  sel_anchor_line; /**< line_from_bottom where drag started.    */
        int  sel_anchor_col;  /**< 1-based codepoint col where drag start. */
        int  sel_end_line;    /**< line_from_bottom of current cursor.     */
        int  sel_end_col;     /**< 1-based codepoint col of current cursor.*/

        /* scrollback view state */
        int  sb_offset; /**< 0 = live view, >0 = scrolled back.       */
        bool sb_redraw; /**< Deferred scrollback repaint pending.      */

        /* Tier 1 — throughput sparkline */
        uint64_t rx_bps_hist[ZT_SPARK_HIST];
        int      rx_bps_head;
        double   rx_bps;
        uint64_t rx_bytes_last;
    } tui;

    struct {
        int           fd;     /**< Log-file FD, or -1 when no active log.   */
        const char   *path;   /**< Log file path.                           */
        zt_log_format format; /**< Log format (text, JSON, raw).            */
        uint64_t      bytes;
        uint64_t      max_bytes; /**< 0 = no rotation.                         */

        unsigned char line[ZT_LINEBUF_CAP];
        size_t        line_len;
        bool          line_start;

        unsigned      hex_col;
        bool          hex_mode;
        unsigned char hex_row[16]; /**< Accumulator for current hex dump row.   */
        unsigned      hex_row_len; /**< Valid bytes in hex_row (0..16).          */
        uint64_t      hex_offset;  /**< Running byte offset for hex dump addr.  */

        /* scrollback ring buffer */
        char **sb_lines;
        int    sb_count;
        int    sb_head;

        char  *watch[ZT_WATCH_MAX];
        int    watch_count;
        bool   watch_beep;

        /* Tier 3 — bookmarks */
        int   bookmarks[ZT_BOOKMARK_MAX]; /**< Scrollback offsets.             */
        char *bookmark_notes[ZT_BOOKMARK_MAX];
        int   bookmark_count;

        /* Tier 3 — log-level volume */
        bool mute_dbg;
        bool mute_inf;

        /* asciinema cast v2 recording (--rec) */
        const char *rec_path;
    } log;

    struct {
        /* Tier 3 — metrics */
        int   metrics_fd; /**< Unix listening socket (-1 = off).        */
        char *metrics_path;

        /* Tier 3 — HTTP/SSE/WS bridge */
        int   http_fd; /**< TCP listening socket (-1 = off).         */
        int   http_port;
        void *http_impl;    /**< Opaque — defined in http.c.              */
        char *http_webroot; /**< User static-files dir ('\0' = built-in). */
        bool  http_cors;    /**< Emit permissive CORS headers.            */
        char *http_token;   /**< Bearer token gating POST /tx (NULL=off).  */

        /* Tier 3 — session (detach/attach) */
        int   session_fd; /**< Unix socket (for --detach host).         */
        char *session_name;
    } net;

    struct {
        /* Tier 2 — framing + CRC */
        zt_frame_mode mode;
        unsigned char buf[ZT_LINEBUF_CAP];
        size_t        len;
        bool          escape; /**< SLIP/HDLC escape state.                  */
        uint32_t      rx_count;
        uint32_t      crc_err;

        /* Per-decoder accumulator state. Previously these lived in
         * file-static variables inside framing.c, which meant
         * (a) framing_reset() couldn't clear them on mode change, and
         * (b) the multi-pane code shared the same static storage across
         * panes, blending frames between devices. Hosting them in
         * @c proto keeps each session self-contained and cleanly
         * reset-able. */
        size_t        cobs_pending;  /**< Raw bytes buffered before 0x00 delimiter. */
        unsigned char len16_lenb[2]; /**< Two-byte little-endian length header.    */
        int           len16_have;    /**< 0/1/2 bytes of length header consumed.   */
        size_t        len16_need;    /**< Decoded payload length to collect.       */

        zt_crc_mode   crc_mode;
        bool          crc_append; /**< Auto-append CRC to TX frames.            */

        /* Tier 2 — device-RX SGR-only filter (ADR-0009). When on (default),
         * only well-formed SGR (CSI … m, params 0-9 ; :) reaches the
         * terminal; every other escape is neutralized to caret notation.
         * Off → strict default-deny (all escapes neutralized). @c passthrough
         * wins over this. */
        bool          sgr_passthrough; /**< SGR-filter mode (default ON).      */
        zt_sgr_parser sgr;             /**< Bounded CSI parser, cross-chunk.   */
        bool          sgr_in_line;     /**< Device SGR emitted on the current
                                            line → append \033[0m at flush.    */

        /* Tier 2 — KGDB / raw (transparent) passthrough */
        bool    passthrough;   /**< Transparent relay: raw stdin↔device, TUI off. */
        uint8_t pt_line_state; /**< `~.` line-start exit detector (0/1).           */

        /* Tier 2 — "trusted device" legacy mode (opt-in via --trusted). Restores
         * the pre-ZT-003 behaviour for a device you trust: device escapes render
         * RAW into the managed view (native colour + completion columns, no SGR
         * filter — @c trusted selects ESC_RAW in render_rx), and Tab-completion is
         * mirrored by an immediate echo capture (@c tab_echo) instead of the
         * reconciliation model — fast, like the legacy build, at the cost of the
         * ZT-003 escape protections. See INVARIANTS §6. */
        bool   trusted;  /**< Raw device rendering + immediate Tab capture.   */
        bool   tab_echo; /**< Capturing the device completion echo into input.*/
        size_t tab_skip; /**< Echoed sent-prefix bytes still to skip.         */

        /* Tier 2 — device prompt-line model for Tab-completion reconcile (ADR-0010). */
        zt_devline devline;

        /* Tier 4 — clipboard (OSC 52) */
        bool osc52_enabled;

        /* Tier 4 — OSC 8 hyperlink detection */
        bool hyperlinks;

        bool local_echo;
        bool color_on;
        bool show_ts;
        bool tx_ts;
        bool tx_line_start;

        /* Tier 1 — line-ending translation (--map-out / --map-in). */
        zt_eol_map   map_out;
        zt_eol_map   map_in;
        zt_eol_state eol_state_out;
        zt_eol_state eol_state_in;
    } proto;

    struct {
        /* Tier 2 — filter subprocess */
        int   filter_stdin_fd;  /**< We write RX bytes here.                  */
        int   filter_stdout_fd; /**< We read transformed bytes back.          */
        int   filter_pid;       /**< Child PID; 0 when not active.            */
        char *filter_cmd;       /**< Shell cmdline e.g. "jq .".               */
        char *macros[ZT_MACRO_COUNT];

        /* config hot-reload (--profile + inotify); see ext/profile_watch.c */
        const char *profile_name;
        int         profile_inotify_fd;
        int         profile_inotify_wd;

        /* event hooks (--on-match / --on-connect / --on-disconnect).
         * Opaque to ctx consumers; managed entirely by ext/hooks.c. */
        void *hooks;
    } ext;

    struct {
        /* Global stats, times and control */
        uint64_t        rx_bytes, tx_bytes, rx_lines;
        struct timespec t_start, t_last_hud, t_last_paint;
        bool            paused;
        bool            reconnect;

        /* replay-from-file mode */
        const char *replay_path;
        double      replay_speed;
    } core;
} zt_ctx;

#endif /* ZYTERM_ZT_CTX_H */
