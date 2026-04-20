/**
 * @file profile_watch.c
 * @brief Linux inotify-based config hot-reload for `--profile`.
 *
 * When zyterm starts with `--profile NAME`, this module watches the
 * resolved profile file for edits. On change we re-load the profile
 * (delegated to @ref profile_load) so users tweaking macros, watch
 * patterns, or line-ending modes see them apply without restarting.
 *
 * Watch strategy: we watch the *parent directory* (not the file
 * itself). Editors save via atomic rename ("write tmp, rename onto
 * final path"), which immediately invalidates a file-level inotify
 * watch (the inode the kernel was tracking is gone). Watching the
 * directory and filtering events by basename is the standard fix and
 * survives both atomic-rename and in-place truncate-write flows.
 *
 * Event mask: IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE — these cover
 * vim, neovim (writebackup), helix, vscode, nano, sed -i, sponge,
 * cp/mv, and direct fopen("w") writers.
 *
 * Debounce: edits often fire multiple events in <50 ms (e.g. vim does
 * a CREATE + CLOSE_WRITE + MOVED_TO sequence on `:w`). We coalesce
 * events arriving within 200 ms and apply a single re-load.
 *
 * Runtime safety: per planning/04-FEATURES.md F6, only "runtime-safe"
 * keys take effect immediately (macros, watch patterns, line endings,
 * log level). Profile fields like `baud`/`device`/`frame` are still
 * loaded into the ctx but won't reach the live serial fd until the
 * user reconnects — we surface a notice in the scrollback so they
 * know.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

/* Track basename + last-event time as file-statics. There's only ever
 * one watched profile (--profile takes a single argument). */
static char            s_basename[256] = {0};
static struct timespec s_last_event    = {0};

/* 200 ms debounce window — long enough to coalesce vim's
 * CREATE/CLOSE_WRITE/MOVED_TO burst on :w, short enough to feel
 * instant when the user hits save. */
#define ZT_PROFILE_DEBOUNCE_MS 200

static bool within_debounce(const struct timespec *now_ts) {
    if (s_last_event.tv_sec == 0 && s_last_event.tv_nsec == 0) return false;
    long ms = (now_ts->tv_sec - s_last_event.tv_sec) * 1000L
            + (now_ts->tv_nsec - s_last_event.tv_nsec) / 1000000L;
    return ms < ZT_PROFILE_DEBOUNCE_MS;
}

int profile_watch_start(zt_ctx *c, const char *name) {
    if (!c || !name || !*name) return -1;
    if (c->ext.profile_inotify_fd > 0) profile_watch_stop(c);

    char path[PATH_MAX];
    zt_profile_path(name, path, sizeof path);

    /* Split into dir + basename. The trailing component is what
     * inotify events will report in struct inotify_event::name. */
    char *slash = strrchr(path, '/');
    if (!slash) {
        errno = EINVAL;
        return -1;
    }
    *slash             = '\0';
    const char *dir    = path;
    const char *basenm = slash + 1;
    snprintf(s_basename, sizeof s_basename, "%s", basenm);

    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return -1;

    int wd = inotify_add_watch(fd, dir,
                               IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
    if (wd < 0) {
        int saved = errno;
        close(fd);
        errno     = saved;
        return -1;
    }

    c->ext.profile_inotify_fd = fd;
    c->ext.profile_inotify_wd = wd;
    c->ext.profile_name       = name;
    log_notice(c, "config hot-reload: watching %s/%s", dir, basenm);
    return 0;
}

void profile_watch_tick(zt_ctx *c) {
    if (!c || c->ext.profile_inotify_fd <= 0) return;

    /* Drain all queued events in one syscall. The buffer must be
     * large enough for at least one event (sizeof + NAME_MAX + 1)
     * per kernel docs; a single 4 KiB page comfortably holds the
     * editor-save burst we typically see. */
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t n = read(c->ext.profile_inotify_fd, buf, sizeof buf);
    if (n <= 0) return; /* EAGAIN / no events */

    bool match = false;
    for (char *p = buf; p < buf + n;) {
        const struct inotify_event *ev = (const struct inotify_event *) p;
        if (ev->len > 0 && strcmp(ev->name, s_basename) == 0) match = true;
        p += sizeof(struct inotify_event) + ev->len;
    }
    if (!match) return;

    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    if (within_debounce(&now_ts)) {
        s_last_event = now_ts;
        return;
    }
    s_last_event = now_ts;

    if (profile_load(c, c->ext.profile_name) == 0) {
        log_notice(c,
                   "config reloaded: %s "
                   "(non-runtime-safe keys e.g. baud/device take effect on next reconnect)",
                   c->ext.profile_name);
        c->tui.ui_dirty = true;
    } else {
        log_notice(c, "config reload failed: %s", c->ext.profile_name);
    }
}

void profile_watch_stop(zt_ctx *c) {
    if (!c || c->ext.profile_inotify_fd <= 0) return;
    if (c->ext.profile_inotify_wd > 0)
        inotify_rm_watch(c->ext.profile_inotify_fd, c->ext.profile_inotify_wd);
    close(c->ext.profile_inotify_fd);
    c->ext.profile_inotify_fd = 0;
    c->ext.profile_inotify_wd = 0;
}
