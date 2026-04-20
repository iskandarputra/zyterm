/**
 * @file clipboard.c
 * @brief Native X11 clipboard owner — runtime-loaded, zero build deps.
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
 * Runtime loading (dlopen)
 * ────────────────────────
 * Instead of requiring libxcb-dev at compile time, we dlopen
 * libxcb.so.1 at first use and resolve the ~15 functions we need
 * through dlsym(). libxcb.so.1 is present on virtually every
 * graphical Linux desktop (X11 and XWayland) even without -dev
 * packages, because it's a runtime dependency of GTK, Qt, Mesa, etc.
 *
 * If the library isn't available (headless, pure Wayland without
 * XWayland, BSD without xcb), clipboard_native_set() returns false
 * and callers fall through to OSC 52 + external-helper + file
 * fallbacks. True zero dependency: no -lxcb, no -lxcb-xfixes,
 * no headers, no pkg-config probe.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ─── Minimal xcb type definitions ──────────────────────────────────────────
 * We define just enough of the xcb ABI to drive a selection owner.
 * These match the X11 wire protocol (stable since 1987) and the xcb
 * struct layouts generated from xcb-proto XML. Field order and padding
 * follow the X11 event wire format: all events are exactly 32 bytes. */

typedef struct zt_xcb_connection_t zt_xcb_connection_t; /* opaque */
typedef struct zt_xcb_setup_t     zt_xcb_setup_t;      /* opaque */
typedef uint32_t zt_xcb_window_t;
typedef uint32_t zt_xcb_atom_t;
typedef uint32_t zt_xcb_timestamp_t;
typedef uint32_t zt_xcb_visualid_t;
typedef uint32_t zt_xcb_colormap_t;

typedef struct { unsigned int sequence; } zt_xcb_void_cookie_t;
typedef struct { unsigned int sequence; } zt_xcb_intern_atom_cookie_t;

typedef struct {
    uint8_t     response_type;
    uint8_t     pad0;
    uint16_t    sequence;
    uint32_t    length;
    zt_xcb_atom_t atom;
} zt_xcb_intern_atom_reply_t;

typedef struct {
    uint8_t  response_type;
    uint8_t  pad0;
    uint16_t sequence;
    uint32_t pad[7];
    uint32_t full_sequence;
} zt_xcb_generic_event_t;

typedef struct {
    uint8_t  response_type;
    uint8_t  pad0;
    uint16_t sequence;
    uint32_t length;
    /* ... more fields we don't access */
} zt_xcb_generic_error_t;

/* SelectionRequest — 32 bytes (X event code 30) */
typedef struct {
    uint8_t           response_type;
    uint8_t           pad0;
    uint16_t          sequence;
    zt_xcb_timestamp_t time;
    zt_xcb_window_t   owner;
    zt_xcb_window_t   requestor;
    zt_xcb_atom_t     selection;
    zt_xcb_atom_t     target;
    zt_xcb_atom_t     property;
    uint8_t           pad1[4]; /* pad to 32 bytes */
} zt_sel_request_t;

/* SelectionClear — 32 bytes (X event code 29) */
typedef struct {
    uint8_t           response_type;
    uint8_t           pad0;
    uint16_t          sequence;
    zt_xcb_timestamp_t time;
    zt_xcb_window_t   owner;
    zt_xcb_atom_t     selection;
    uint8_t           pad1[16]; /* pad to 32 bytes */
} zt_sel_clear_t;

/* SelectionNotify — 32 bytes (X event code 31) */
typedef struct {
    uint8_t           response_type;
    uint8_t           pad0;
    uint16_t          sequence;
    zt_xcb_timestamp_t time;
    zt_xcb_window_t   requestor;
    zt_xcb_atom_t     selection;
    zt_xcb_atom_t     target;
    zt_xcb_atom_t     property;
    uint8_t           pad1[8]; /* pad to 32 bytes */
} zt_sel_notify_t;

typedef struct {
    zt_xcb_window_t   root;
    zt_xcb_colormap_t default_colormap;
    uint32_t          white_pixel;
    uint32_t          black_pixel;
    uint32_t          current_input_masks;
    uint16_t          width_in_pixels;
    uint16_t          height_in_pixels;
    uint16_t          width_in_millimeters;
    uint16_t          height_in_millimeters;
    uint16_t          min_installed_maps;
    uint16_t          max_installed_maps;
    zt_xcb_visualid_t root_visual;
    uint8_t           backing_stores;
    uint8_t           save_unders;
    uint8_t           root_depth;
    uint8_t           allowed_depths_len;
} zt_xcb_screen_t;

typedef struct {
    zt_xcb_screen_t *data;
    int              rem;
    int              index;
} zt_xcb_screen_iterator_t;

/* X11 constants we need. */
enum {
    ZT_XCB_PROP_MODE_REPLACE   = 0,
    ZT_XCB_COPY_FROM_PARENT    = 0,
    ZT_XCB_NONE                = 0,
    ZT_XCB_CURRENT_TIME        = 0,
    ZT_XCB_CW_EVENT_MASK       = 2048,    /* 0x800 */
    ZT_XCB_EVENT_MASK_PROP_CHG = 4194304, /* 0x400000 */
    ZT_XCB_EVENT_MASK_NONE     = 0,
    ZT_XCB_WIN_CLASS_INPUT_ONLY= 2,
    ZT_XCB_SELECTION_REQUEST   = 30,
    ZT_XCB_SELECTION_CLEAR     = 29,
    ZT_XCB_SELECTION_NOTIFY    = 31,
};

/* ─── Function pointer table (filled once by xcb_load()) ──────────────── */

typedef struct {
    void *handle; /* dlopen handle */

    zt_xcb_connection_t *(*connect)(const char *, int *);
    void                 (*disconnect)(zt_xcb_connection_t *);
    int                  (*connection_has_error)(zt_xcb_connection_t *);
    const zt_xcb_setup_t *(*get_setup)(zt_xcb_connection_t *);
    zt_xcb_screen_iterator_t (*setup_roots_iterator)(const zt_xcb_setup_t *);
    uint32_t             (*generate_id)(zt_xcb_connection_t *);
    zt_xcb_void_cookie_t (*create_window)(zt_xcb_connection_t *, uint8_t,
                          zt_xcb_window_t, zt_xcb_window_t,
                          int16_t, int16_t, uint16_t, uint16_t,
                          uint16_t, uint16_t, zt_xcb_visualid_t,
                          uint32_t, const void *);
    int                  (*flush)(zt_xcb_connection_t *);
    int                  (*get_file_descriptor)(zt_xcb_connection_t *);
    zt_xcb_intern_atom_cookie_t (*intern_atom)(zt_xcb_connection_t *,
                                 uint8_t, uint16_t, const char *);
    zt_xcb_intern_atom_reply_t *(*intern_atom_reply)(zt_xcb_connection_t *,
                                  zt_xcb_intern_atom_cookie_t,
                                  zt_xcb_generic_error_t **);
    zt_xcb_void_cookie_t (*set_selection_owner)(zt_xcb_connection_t *,
                          zt_xcb_window_t, zt_xcb_atom_t,
                          zt_xcb_timestamp_t);
    zt_xcb_void_cookie_t (*change_property)(zt_xcb_connection_t *,
                          uint8_t, zt_xcb_window_t, zt_xcb_atom_t,
                          zt_xcb_atom_t, uint8_t, uint32_t,
                          const void *);
    zt_xcb_void_cookie_t (*send_event)(zt_xcb_connection_t *,
                          uint8_t, zt_xcb_window_t, uint32_t,
                          const char *);
    zt_xcb_generic_event_t *(*poll_for_event)(zt_xcb_connection_t *);
} xcb_api_t;

/* Load libxcb.so.1 and resolve every symbol we need.
 * Returns true on success, false if *any* symbol is missing. */
static bool xcb_load(xcb_api_t *api) {
    api->handle = dlopen("libxcb.so.1", RTLD_LAZY);
    if (!api->handle) {
        /* Try unversioned name as a fallback. */
        api->handle = dlopen("libxcb.so", RTLD_LAZY);
    }
    if (!api->handle) return false;

#define LOAD(fn)                                                          \
    do {                                                                  \
        *(void **)(&api->fn) = dlsym(api->handle, "xcb_" #fn);           \
        if (!api->fn) { dlclose(api->handle); api->handle = NULL; return false; } \
    } while (0)

    LOAD(connect);
    LOAD(disconnect);
    LOAD(connection_has_error);
    LOAD(get_setup);
    LOAD(setup_roots_iterator);
    LOAD(generate_id);
    LOAD(create_window);
    LOAD(flush);
    LOAD(get_file_descriptor);
    LOAD(intern_atom);
    LOAD(intern_atom_reply);
    LOAD(set_selection_owner);
    LOAD(change_property);
    LOAD(send_event);
    LOAD(poll_for_event);

#undef LOAD
    return true;
}

/* ─── Atom names ────────────────────────────────────────────────────────── */

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
    A_INCR,
    A_ZYTERM_DATA,
    ATOMS_END
};

static const char *const k_atom_names[ATOMS_END] = {
    "CLIPBOARD", "PRIMARY", "TARGETS",    "UTF8_STRING",
    "STRING",    "TEXT",    "text/plain",  "text/plain;charset=utf-8",
    "TIMESTAMP", "INCR",   "ZYTERM_DATA",
};

/* ─── Shared state ──────────────────────────────────────────────────────── */

static struct {
    pthread_mutex_t mu;
    pthread_t       tid;
    bool            running;
    bool            init_failed;

    char  *buf;
    size_t len;
    bool   need_claim;

    int wakefd[2];

    /* xcb resources */
    zt_xcb_connection_t *conn;
    zt_xcb_window_t      window;
    zt_xcb_atom_t        atoms[ATOMS_END];

    /* runtime-loaded API */
    xcb_api_t api;
} g = {.mu = PTHREAD_MUTEX_INITIALIZER, .wakefd = {-1, -1}};

/* ─── Helpers ───────────────────────────────────────────────────────────── */

static void wake_worker(void) {
    if (g.wakefd[1] < 0) return;
    char b = 'w';
    while (write(g.wakefd[1], &b, 1) < 0 && errno == EINTR) {}
}

static void reply_selection(zt_sel_request_t *req, zt_xcb_atom_t target,
                            const void *data, size_t len) {
    zt_xcb_atom_t prop = req->property ? req->property : req->target;
    g.api.change_property(g.conn, ZT_XCB_PROP_MODE_REPLACE, req->requestor,
                          prop, target,
                          target == g.atoms[A_TARGETS] ? 32 : 8,
                          (uint32_t)len, data);

    zt_sel_notify_t ev = {0};
    ev.response_type   = ZT_XCB_SELECTION_NOTIFY;
    ev.time            = req->time;
    ev.requestor       = req->requestor;
    ev.selection       = req->selection;
    ev.target          = req->target;
    ev.property        = prop;
    g.api.send_event(g.conn, 0, req->requestor, ZT_XCB_EVENT_MASK_NONE,
                     (const char *)&ev);
}

static void reject_selection(zt_sel_request_t *req) {
    zt_sel_notify_t ev = {0};
    ev.response_type   = ZT_XCB_SELECTION_NOTIFY;
    ev.time            = req->time;
    ev.requestor       = req->requestor;
    ev.selection       = req->selection;
    ev.target          = req->target;
    ev.property        = ZT_XCB_NONE;
    g.api.send_event(g.conn, 0, req->requestor, ZT_XCB_EVENT_MASK_NONE,
                     (const char *)&ev);
}

static void handle_selection_request(zt_sel_request_t *req) {
    pthread_mutex_lock(&g.mu);
    char  *snap_buf = g.buf ? memcpy(malloc(g.len), g.buf, g.len) : NULL;
    size_t snap_len = g.len;
    pthread_mutex_unlock(&g.mu);

    if (req->target == g.atoms[A_TARGETS]) {
        zt_xcb_atom_t list[] = {
            g.atoms[A_TARGETS],    g.atoms[A_TIMESTAMP],
            g.atoms[A_UTF8_STRING], g.atoms[A_STRING],
            g.atoms[A_TEXT],       g.atoms[A_TEXT_PLAIN],
            g.atoms[A_TEXT_PLAIN_UTF8],
        };
        reply_selection(req, g.atoms[A_TARGETS], list,
                        sizeof list / sizeof list[0]);
    } else if (req->target == g.atoms[A_UTF8_STRING] ||
               req->target == g.atoms[A_STRING]      ||
               req->target == g.atoms[A_TEXT]         ||
               req->target == g.atoms[A_TEXT_PLAIN]   ||
               req->target == g.atoms[A_TEXT_PLAIN_UTF8]) {
        if (snap_buf && snap_len > 0)
            reply_selection(req, req->target, snap_buf, snap_len);
        else
            reject_selection(req);
    } else {
        reject_selection(req);
    }

    free(snap_buf);
    g.api.flush(g.conn);
}

/* ─── Worker thread ─────────────────────────────────────────────────────── */

static void *worker_main(void *arg) {
    (void)arg;

    g.conn = g.api.connect(NULL, NULL);
    if (!g.conn || g.api.connection_has_error(g.conn)) {
        pthread_mutex_lock(&g.mu);
        g.init_failed = true;
        g.running     = false;
        pthread_mutex_unlock(&g.mu);
        return NULL;
    }

    /* Intern all atoms in one round-trip batch. */
    zt_xcb_intern_atom_cookie_t cookies[ATOMS_END];
    for (int i = 0; i < ATOMS_END; i++)
        cookies[i] = g.api.intern_atom(g.conn, 0,
                         (uint16_t)strlen(k_atom_names[i]),
                         k_atom_names[i]);
    for (int i = 0; i < ATOMS_END; i++) {
        zt_xcb_intern_atom_reply_t *r =
            g.api.intern_atom_reply(g.conn, cookies[i], NULL);
        if (r) {
            g.atoms[i] = r->atom;
            free(r);
        }
    }

    const zt_xcb_setup_t     *setup = g.api.get_setup(g.conn);
    zt_xcb_screen_iterator_t  it    = g.api.setup_roots_iterator(setup);
    if (!it.data) {
        g.api.disconnect(g.conn);
        g.conn        = NULL;
        g.init_failed = true;
        g.running     = false;
        return NULL;
    }
    zt_xcb_screen_t *screen = it.data;

    g.window        = g.api.generate_id(g.conn);
    uint32_t mask   = ZT_XCB_CW_EVENT_MASK;
    uint32_t vals[] = {ZT_XCB_EVENT_MASK_PROP_CHG};
    g.api.create_window(g.conn, ZT_XCB_COPY_FROM_PARENT, g.window,
                        screen->root, -10, -10, 1, 1, 0,
                        ZT_XCB_WIN_CLASS_INPUT_ONLY, screen->root_visual,
                        mask, vals);
    g.api.flush(g.conn);

    int xfd = g.api.get_file_descriptor(g.conn);

    for (;;) {
        pthread_mutex_lock(&g.mu);
        bool need_claim = g.need_claim;
        g.need_claim    = false;
        pthread_mutex_unlock(&g.mu);
        if (need_claim) {
            g.api.set_selection_owner(g.conn, g.window,
                                     g.atoms[A_CLIPBOARD],
                                     ZT_XCB_CURRENT_TIME);
            g.api.flush(g.conn);
        }

        struct pollfd pfds[2] = {{xfd, POLLIN, 0}, {g.wakefd[0], POLLIN, 0}};
        int n = poll(pfds, 2, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfds[1].revents & POLLIN) {
            char drain[64];
            while (read(g.wakefd[0], drain, sizeof drain) > 0) {}
        }

        for (zt_xcb_generic_event_t *ev;
             (ev = g.api.poll_for_event(g.conn));) {
            switch (ev->response_type & 0x7f) {
            case ZT_XCB_SELECTION_REQUEST:
                handle_selection_request((zt_sel_request_t *)ev);
                break;
            case ZT_XCB_SELECTION_CLEAR: {
                zt_sel_clear_t *ce = (zt_sel_clear_t *)ev;
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
        if (g.api.connection_has_error(g.conn)) break;
    }

    g.api.disconnect(g.conn);
    g.conn = NULL;
    return NULL;
}

/* ─── Public API ────────────────────────────────────────────────────────── */

bool clipboard_native_set(const char *buf, size_t n) {
    if (!buf || n == 0) return false;

    /* No X server → nothing to own. Covers headless, SSH, and
     * pure-Wayland without XWayland. */
    const char *display = getenv("DISPLAY");
    if (!display || !*display) return false;

    pthread_mutex_lock(&g.mu);
    if (g.init_failed) {
        pthread_mutex_unlock(&g.mu);
        return false;
    }

    /* First call: try to load libxcb at runtime. */
    if (!g.running && !g.api.handle) {
        if (!xcb_load(&g.api)) {
            g.init_failed = true;
            pthread_mutex_unlock(&g.mu);
            return false;
        }
    }

    if (!g.running) {
        if (pipe(g.wakefd) < 0) {
            g.init_failed = true;
            pthread_mutex_unlock(&g.mu);
            return false;
        }
        fcntl(g.wakefd[0], F_SETFL, O_NONBLOCK);
        fcntl(g.wakefd[1], F_SETFL, O_NONBLOCK);
        if (pthread_create(&g.tid, NULL, worker_main, NULL) != 0) {
            close(g.wakefd[0]);
            close(g.wakefd[1]);
            g.wakefd[0] = g.wakefd[1] = -1;
            g.init_failed = true;
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
