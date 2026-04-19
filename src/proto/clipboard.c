/**
 * @file clipboard.c
 * @brief Native X11 clipboard owner (no external helper required).
 *
 * Why this exists
 * ───────────────
 * X11's CLIPBOARD selection is owned by a *live* process: when an app
 * pastes, the X server forwards a SelectionRequest event to whoever
 * called XSetSelectionOwner most recently, and that process must reply
 * with the bytes via xcb_send_event/SelectionNotify. If the owner
 * exits, the clipboard is empty.
 *
 * Without this module zyterm depends on `xclip`/`xsel`/`wl-copy` or
 * the host terminal honoring OSC 52 — neither of which is universally
 * available. With this module we:
 *
 *   1. open a *second* X connection on a background thread (separate
 *      from any X-aware code in the host TTY, so we don't conflict),
 *   2. create an InputOnly off-screen window to act as selection owner,
 *   3. claim CLIPBOARD ownership and serve SelectionRequest events
 *      with our buffered text until either the process exits or
 *      another client steals the selection (SelectionClear).
 *
 * Build wiring
 * ────────────
 * The Makefile probes pkg-config for `xcb` + `xcb-xfixes` and defines
 * @c ZT_HAVE_X11 / links the libs only when present. On systems
 * without xcb (headless, BSD without the dev pkgs, etc.) the stubs at
 * the bottom of this file return false and callers fall through to
 * OSC 52 + helper-binary fallbacks. Zero hard build dep preserved.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef ZT_HAVE_X11

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

/* Atom names we need. Order kept stable so atoms[] indexes are
 * predictable; ATOMS_END marks the count. */
enum {
    A_CLIPBOARD = 0,
    A_PRIMARY,
    A_TARGETS,
    A_UTF8_STRING,
    A_STRING,
    A_TEXT,
    A_TEXT_PLAIN,
    A_TEXT_PLAIN_UTF8,
    A_TIMESTAMP,
    A_INCR, /* unused but reserved for huge buffers */
    A_ZYTERM_DATA,
    ATOMS_END
};

static const char *const k_atom_names[ATOMS_END] = {
    "CLIPBOARD", "PRIMARY", "TARGETS",     "UTF8_STRING",
    "STRING",    "TEXT",    "text/plain",  "text/plain;charset=utf-8",
    "TIMESTAMP", "INCR",    "ZYTERM_DATA",
};

/* Shared state between callers (set/clear) and the worker thread.
 * The worker holds the X connection. set() copies new data under
 * `mu`, then writes one byte to `wakefd[1]` so the worker's poll()
 * returns and it re-claims ownership with the new buffer. */
static struct {
    pthread_mutex_t mu;
    pthread_t       tid;
    bool            running;
    bool            init_failed;

    /* Buffer currently advertised on the CLIPBOARD selection. The
     * worker reads under mu and copies the bytes into each
     * SelectionNotify reply. Cleared on shutdown. */
    char  *buf;
    size_t len;
    bool   need_claim; /* set() raises this; worker calls
                        * xcb_set_selection_owner and clears it. */

    /* Self-pipe used to break out of poll() / xcb_wait_for_event. */
    int wakefd[2];

    /* X resources owned by the worker. */
    xcb_connection_t *conn;
    xcb_window_t      window;
    xcb_atom_t        atoms[ATOMS_END];
} g = {.mu = PTHREAD_MUTEX_INITIALIZER, .wakefd = {-1, -1}};

/* Send one byte into the wake pipe; ignores EAGAIN on a full pipe
 * because the worker is already pending-wake in that case. */
static void wake_worker(void) {
    if (g.wakefd[1] < 0) return;
    char b = 'w';
    while (write(g.wakefd[1], &b, 1) < 0 && errno == EINTR) {}
}

/* Reply to a SelectionRequest by writing `data` of `len` bytes onto
 * `requestor.property` with type `target`, then sending a
 * SelectionNotify event so the requestor knows the property is ready.
 *
 * If `target == TARGETS` we instead advertise the list of formats we
 * can serve so requestors can negotiate. */
static void reply_selection(xcb_selection_request_event_t *req, xcb_atom_t target,
                            const void *data, size_t len) {
    xcb_atom_t prop = req->property ? req->property : req->target;
    xcb_change_property(g.conn, XCB_PROP_MODE_REPLACE, req->requestor, prop, target,
                        target == g.atoms[A_TARGETS] ? 32 : 8, (uint32_t)len, data);

    xcb_selection_notify_event_t ev = {0};
    ev.response_type                = XCB_SELECTION_NOTIFY;
    ev.time                         = req->time;
    ev.requestor                    = req->requestor;
    ev.selection                    = req->selection;
    ev.target                       = req->target;
    ev.property                     = prop;
    xcb_send_event(g.conn, 0, req->requestor, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
}

/* "I can't serve this format" — reply with property=None per ICCCM. */
static void reject_selection(xcb_selection_request_event_t *req) {
    xcb_selection_notify_event_t ev = {0};
    ev.response_type                = XCB_SELECTION_NOTIFY;
    ev.time                         = req->time;
    ev.requestor                    = req->requestor;
    ev.selection                    = req->selection;
    ev.target                       = req->target;
    ev.property                     = XCB_NONE;
    xcb_send_event(g.conn, 0, req->requestor, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
}

/* Service one SelectionRequest. Reads the buffer under mu so set()
 * can run concurrently from another thread without tearing. */
static void handle_selection_request(xcb_selection_request_event_t *req) {
    pthread_mutex_lock(&g.mu);
    char  *snap_buf = g.buf ? memcpy(malloc(g.len), g.buf, g.len) : NULL;
    size_t snap_len = g.len;
    pthread_mutex_unlock(&g.mu);

    if (req->target == g.atoms[A_TARGETS]) {
        /* Tell the requestor what we serve. Order matters: most
         * preferred (UTF-8) first. */
        xcb_atom_t list[] = {
            g.atoms[A_TARGETS],         g.atoms[A_TIMESTAMP], g.atoms[A_UTF8_STRING],
            g.atoms[A_STRING],          g.atoms[A_TEXT],      g.atoms[A_TEXT_PLAIN],
            g.atoms[A_TEXT_PLAIN_UTF8],
        };
        reply_selection(req, g.atoms[A_TARGETS], list, sizeof list / sizeof list[0]);
    } else if (req->target == g.atoms[A_UTF8_STRING] || req->target == g.atoms[A_STRING] ||
               req->target == g.atoms[A_TEXT] || req->target == g.atoms[A_TEXT_PLAIN] ||
               req->target == g.atoms[A_TEXT_PLAIN_UTF8]) {
        if (snap_buf && snap_len > 0) {
            reply_selection(req, req->target, snap_buf, snap_len);
        } else {
            reject_selection(req);
        }
    } else {
        reject_selection(req);
    }

    free(snap_buf);
    xcb_flush(g.conn);
}

/* Worker thread: open conn, intern atoms, create window, then loop
 * forever multiplexing wake-pipe + xcb fd via poll(). */
static void *worker_main(void *arg) {
    (void)arg;

    g.conn = xcb_connect(NULL, NULL);
    if (!g.conn || xcb_connection_has_error(g.conn)) {
        pthread_mutex_lock(&g.mu);
        g.init_failed = true;
        g.running     = false;
        pthread_mutex_unlock(&g.mu);
        return NULL;
    }

    /* Intern all atoms in one round-trip batch. */
    xcb_intern_atom_cookie_t cookies[ATOMS_END];
    for (int i = 0; i < ATOMS_END; i++) {
        cookies[i] =
            xcb_intern_atom(g.conn, 0, (uint16_t)strlen(k_atom_names[i]), k_atom_names[i]);
    }
    for (int i = 0; i < ATOMS_END; i++) {
        xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(g.conn, cookies[i], NULL);
        if (r) {
            g.atoms[i] = r->atom;
            free(r);
        }
    }

    const xcb_setup_t    *setup = xcb_get_setup(g.conn);
    xcb_screen_iterator_t it    = xcb_setup_roots_iterator(setup);
    if (!it.data) {
        xcb_disconnect(g.conn);
        g.conn        = NULL;
        g.init_failed = true;
        g.running     = false;
        return NULL;
    }
    xcb_screen_t *screen = it.data;

    /* InputOnly off-screen window. Never mapped — it exists only as
     * a SelectionOwner target so the X server can route
     * SelectionRequest events to this connection. */
    g.window         = xcb_generate_id(g.conn);
    uint32_t mask    = XCB_CW_EVENT_MASK;
    uint32_t vals[1] = {XCB_EVENT_MASK_PROPERTY_CHANGE};
    xcb_create_window(g.conn, XCB_COPY_FROM_PARENT, g.window, screen->root, -10, -10, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_ONLY, screen->root_visual, mask, vals);
    xcb_flush(g.conn);

    int xfd = xcb_get_file_descriptor(g.conn);

    for (;;) {
        /* Re-claim ownership if set() raised the flag. We do it in
         * the worker (not the caller) so all X calls happen on one
         * thread → no need to wrap xcb in a separate mutex. */
        pthread_mutex_lock(&g.mu);
        bool need_claim = g.need_claim;
        g.need_claim    = false;
        pthread_mutex_unlock(&g.mu);
        if (need_claim) {
            /* CLIPBOARD only. We deliberately do NOT claim PRIMARY:
             * other apps treat their highlighted text as the PRIMARY
             * selection, and stealing PRIMARY (or having theirs steal
             * ours via SelectionClear) wipes our buffer at exactly
             * the wrong moment — e.g. user copies in zyterm, opens
             * Notepad, hits Ctrl+A, then Ctrl+V finds an empty
             * clipboard. CLIPBOARD is the explicit-copy buffer and
             * is what Ctrl+V actually reads. */
            xcb_set_selection_owner(g.conn, g.window, g.atoms[A_CLIPBOARD], XCB_CURRENT_TIME);
            xcb_flush(g.conn);
        }

        struct pollfd pfds[2] = {{xfd, POLLIN, 0}, {g.wakefd[0], POLLIN, 0}};
        int           n       = poll(pfds, 2, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfds[1].revents & POLLIN) {
            char drain[64];
            while (read(g.wakefd[0], drain, sizeof drain) > 0) {}
        }
        /* Drain ALL pending events before re-polling — xcb buffers
         * may hold several. */
        for (xcb_generic_event_t *ev; (ev = xcb_poll_for_event(g.conn));) {
            switch (ev->response_type & 0x7f) {
            case XCB_SELECTION_REQUEST:
                handle_selection_request((xcb_selection_request_event_t *)ev);
                break;
            case XCB_SELECTION_CLEAR: {
                /* Only react when CLIPBOARD itself was stolen
                 * (another app called set_selection_owner on it,
                 * usually because the user did an explicit Ctrl+C
                 * elsewhere). PRIMARY clears are routine — every
                 * highlight in every app fires one — and dropping
                 * the buffer on those would empty the clipboard
                 * the moment the user selects anything outside
                 * zyterm. */
                xcb_selection_clear_event_t *ce = (xcb_selection_clear_event_t *)ev;
                if (ce->selection == g.atoms[A_CLIPBOARD]) {
                    pthread_mutex_lock(&g.mu);
                    free(g.buf);
                    g.buf = NULL;
                    g.len = 0;
                    pthread_mutex_unlock(&g.mu);
                }
                break;
            }
            default: break;
            }
            free(ev);
        }
        if (xcb_connection_has_error(g.conn)) break;
    }

    xcb_disconnect(g.conn);
    g.conn = NULL;
    return NULL;
}

bool clipboard_native_set(const char *buf, size_t n) {
    if (!buf || n == 0) return false;
    /* Skip cleanly when there's no X server (SSH session, headless,
     * Wayland w/o XWayland). Saves a libxcb connect attempt. */
    const char *display = getenv("DISPLAY");
    if (!display || !*display) return false;

    pthread_mutex_lock(&g.mu);
    if (g.init_failed) {
        pthread_mutex_unlock(&g.mu);
        return false;
    }
    if (!g.running) {
        if (pipe(g.wakefd) < 0) {
            g.init_failed = true;
            pthread_mutex_unlock(&g.mu);
            return false;
        }
        /* Non-blocking so wake_worker() never stalls under burst. */
        fcntl(g.wakefd[0], F_SETFL, O_NONBLOCK);
        fcntl(g.wakefd[1], F_SETFL, O_NONBLOCK);
        if (pthread_create(&g.tid, NULL, worker_main, NULL) != 0) {
            close(g.wakefd[0]);
            close(g.wakefd[1]);
            g.wakefd[0] = g.wakefd[1] = -1;
            g.init_failed             = true;
            pthread_mutex_unlock(&g.mu);
            return false;
        }
        pthread_detach(g.tid);
        g.running = true;
    }

    char *copy = malloc(n);
    if (!copy) {
        pthread_mutex_unlock(&g.mu);
        return false;
    }
    memcpy(copy, buf, n);
    free(g.buf);
    g.buf        = copy;
    g.len        = n;
    g.need_claim = true;
    pthread_mutex_unlock(&g.mu);

    wake_worker();
    return true;
}

#else /* !ZT_HAVE_X11 */

bool clipboard_native_set(const char *buf, size_t n) {
    (void)buf;
    (void)n;
    return false;
}

#endif
