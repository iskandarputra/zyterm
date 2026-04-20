/**
 * @file    ext.h
 * @brief   Optional extensions — bookmarks, log diff, filter subprocess,
 *          log-level mute, multi-pane, profile load/save, reconnect popup.
 *
 * Module: @c ext/.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#ifndef ZYTERM_INTERNAL_EXT_H_
#define ZYTERM_INTERNAL_EXT_H_

#include "net.h"

/* ── ext/bookmarks.c ───────────────────────────────────────────────────── */
int  bookmark_add(zt_ctx *c, int sb_offset, const char *note);
void bookmark_remove(zt_ctx *c, int idx);
void bookmark_list_draw(zt_ctx *c);
int  bookmark_goto(zt_ctx *c, int idx);

/* ── ext/diff.c ────────────────────────────────────────────────────────── */
int diff_run(const char *a, const char *b);

/* ── ext/filter.c ──────────────────────────────────────────────────────── */
int  filter_start(zt_ctx *c, const char *shell_cmd);
void filter_stop(zt_ctx *c);
void filter_feed(zt_ctx *c, const unsigned char *buf, size_t n);
int  filter_poll_fd(const zt_ctx *c); /**< -1 when inactive. */
void filter_drain(zt_ctx *c);
/* ── ext/hooks.c ────────────────────────────────────────────────────────── */
enum {
    ZT_HOOK_EVENT_MATCH,      /**< Fires when a complete RX line matches a regex.   */
    ZT_HOOK_EVENT_CONNECT,    /**< Fires once after the device opens successfully.   */
    ZT_HOOK_EVENT_DISCONNECT, /**< Fires when the serial fd is closed (HUP/teardown).*/
};

/** Register a hook from a CLI flag. For MATCH the spec is
 *  "/REGEX/=ACTION"; for CONNECT/DISCONNECT it is just "ACTION".
 *  ACTION starting with @c send: injects bytes (escapes resolved
 *  like F-key macros), otherwise it is shelled out via /bin/sh -c.
 *  Returns 0 on success, -1 on parse error or table full. */
int  hooks_register(zt_ctx *c, int event, const char *spec);
/** Match @p line against every MATCH hook (POSIX ERE) and dispatch
 *  the first hit. Cheap when no hooks registered. */
void hooks_on_line(zt_ctx *c, const unsigned char *line, size_t len);
/** Fire all CONNECT or DISCONNECT hooks. */
void hooks_on_event(zt_ctx *c, int event);
/** Reap any zombie hook children. Called from the main loop tick. */
void hooks_reap(zt_ctx *c);
/** Free all hook resources. */
void hooks_free(zt_ctx *c);
/* ── ext/loglevel.c ────────────────────────────────────────────────────── */
bool loglevel_muted(const zt_ctx *c, const unsigned char *line, size_t len);

/* ── ext/multi.c ───────────────────────────────────────────────────────── */
int  multi_start(zt_ctx *primary, const char **extra_devices, int n);
void multi_stop(zt_ctx *c);
void multi_tick(zt_ctx *c);
void multi_render(zt_ctx *c);
void multi_embed_reset(void);

/* ── ext/profile.c ─────────────────────────────────────────────────────── */
int  profile_load(zt_ctx *c, const char *name);
int  profile_save(zt_ctx *c, const char *name);
/** Resolve a profile name to its on-disk path
 *  ("$XDG_CONFIG_HOME/zyterm/<name>.conf" or
 *  "$HOME/.config/zyterm/<name>.conf"). Out-buffer must be >= PATH_MAX. */
void zt_profile_path(const char *name, char *out, size_t cap);

/* ── ext/profile_watch.c ───────────────────────────────────────────────── */
/** Begin watching @p name's profile file (Linux inotify). Subsequent
 *  edits trigger a re-load of runtime-safe keys. Returns 0 on success,
 *  -1 if inotify is unavailable or the path can't be derived. */
int  profile_watch_start(zt_ctx *c, const char *name);
/** Drain any pending inotify events; debounces and applies a re-load
 *  when the watched profile changes. Cheap no-op when inactive. */
void profile_watch_tick(zt_ctx *c);
/** Stop watching and release inotify resources. */
void profile_watch_stop(zt_ctx *c);

/* ── ext/reconnect.c ───────────────────────────────────────────────────── */
void run_reconnect_loop(zt_ctx *c);

#endif /* ZYTERM_INTERNAL_EXT_H_ */
