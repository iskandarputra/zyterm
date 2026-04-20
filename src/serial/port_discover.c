/**
 * @file port_discover.c
 * @brief USB-serial port discovery and re-discovery for hot-plug.
 *
 * Implements `--port-glob` and `--match-vid-pid`. Walks `/sys/class/tty/<name>`
 * to find the USB ancestor, reads `idVendor` / `idProduct` and checks them
 * against user-supplied filters.
 *
 * The classic minicom workflow on Linux falls apart the moment a USB-serial
 * adapter re-enumerates as a different `/dev/ttyUSBn` after a replug. Udev
 * rules can fix it but they're per-host configuration most users never set
 * up. zyterm solves this directly: the originally-supplied path is a hint;
 * on every reconnect the discovery hints are re-evaluated so the same
 * physical adapter gets reattached even if the kernel renamed it.
 *
 * Pure POSIX + sysfs — no libudev dependency. If a future build needs
 * libudev (for richer attribute matching) it can be `dlopen`-loaded the
 * same way `clipboard.c` loads libxcb.
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
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── sysfs reader ─────────────────────────────────────────────────────── */

/** Read a single hex u16 from a sysfs attribute file. Returns 0 on failure. */
static uint16_t read_sysfs_hex(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char buf[16] = {0};
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    return (uint16_t) strtoul(buf, NULL, 16);
}

/** Resolve `/sys/class/tty/<name>/device` to a canonical path and walk
 *  upward until a directory containing both `idVendor` and `idProduct`
 *  is found (the USB device node). Writes the directory into @c out and
 *  returns 0, or -1 if no such ancestor exists. */
static int find_usb_ancestor(const char *device_path, char *out, size_t out_cap) {
    /* /dev/ttyUSB0 → /sys/class/tty/ttyUSB0/device → readlink to absolute */
    const char *base = strrchr(device_path, '/');
    base = base ? base + 1 : device_path;

    char link[PATH_MAX];
    int  k = snprintf(link, sizeof link, "/sys/class/tty/%s/device", base);
    if (k < 0 || (size_t) k >= sizeof link) return -1;

    char real[PATH_MAX];
    if (!realpath(link, real)) return -1;

    /* Walk upward — at most 8 levels — looking for idVendor/idProduct. */
    for (int hops = 0; hops < 8; hops++) {
        char probe[PATH_MAX];
        if ((size_t) snprintf(probe, sizeof probe, "%s/idVendor", real) >= sizeof probe)
            return -1;
        if (access(probe, R_OK) == 0) {
            if ((size_t) snprintf(out, out_cap, "%s", real) >= out_cap) return -1;
            return 0;
        }
        char *slash = strrchr(real, '/');
        if (!slash || slash == real) return -1;
        *slash = '\0';
    }
    return -1;
}

int port_match_vid_pid(const char *device_path, uint16_t vid, uint16_t pid) {
    if (!device_path) return -1;
    if (!vid && !pid) return 1; /* nothing to match → trivially OK */

    char usb_dir[PATH_MAX];
    if (find_usb_ancestor(device_path, usb_dir, sizeof usb_dir) < 0) return -1;

    char vpath[PATH_MAX], ppath[PATH_MAX];
    if ((size_t) snprintf(vpath, sizeof vpath, "%s/idVendor", usb_dir) >= sizeof vpath)
        return -1;
    if ((size_t) snprintf(ppath, sizeof ppath, "%s/idProduct", usb_dir) >= sizeof ppath)
        return -1;

    uint16_t got_vid = read_sysfs_hex(vpath);
    uint16_t got_pid = read_sysfs_hex(ppath);

    if (vid && got_vid != vid) return 0;
    if (pid && got_pid != pid) return 0;
    return 1;
}

/* ── glob-based discovery ─────────────────────────────────────────────── */

char *port_discover(const char *glob_pat, uint16_t vid, uint16_t pid) {
    /* If the caller has no glob, fall back to the conventional candidates. */
    static const char *DEFAULTS[] = {
        "/dev/ttyUSB*",
        "/dev/ttyACM*",
        "/dev/serial/by-id/*",
        NULL,
    };
    const char *patterns[2] = {NULL, NULL};
    int         npat = 0;
    if (glob_pat && *glob_pat) {
        patterns[npat++] = glob_pat;
    } else if (vid || pid) {
        /* No glob, but we have VID/PID — sweep all conventional paths. */
        for (int i = 0; DEFAULTS[i]; i++) {
            glob_t  gb = {0};
            int     gr = glob(DEFAULTS[i], 0, NULL, &gb);
            if (gr == 0) {
                for (size_t j = 0; j < gb.gl_pathc; j++) {
                    if (port_match_vid_pid(gb.gl_pathv[j], vid, pid) == 1) {
                        char *out = strdup(gb.gl_pathv[j]);
                        globfree(&gb);
                        return out;
                    }
                }
            }
            globfree(&gb);
        }
        return NULL;
    } else {
        return NULL; /* nothing to look for */
    }

    glob_t gb = {0};
    int    gr = glob(patterns[0], 0, NULL, &gb);
    if (gr != 0) {
        globfree(&gb);
        return NULL;
    }

    char *winner = NULL;
    for (size_t j = 0; j < gb.gl_pathc; j++) {
        if (vid || pid) {
            if (port_match_vid_pid(gb.gl_pathv[j], vid, pid) != 1) continue;
        }
        winner = strdup(gb.gl_pathv[j]);
        break;
    }
    globfree(&gb);
    return winner;
}

/* ── re-resolve current device path ───────────────────────────────────── */

int port_rediscover(zt_ctx *c) {
    if (!c) return -1;
    if (!c->serial.port_glob && !c->serial.match_vid && !c->serial.match_pid)
        return 0; /* no discovery hints → caller keeps current path */

    char *found = port_discover(c->serial.port_glob,
                                c->serial.match_vid, c->serial.match_pid);
    if (!found) return -1;

    if (c->serial.device && !strcmp(c->serial.device, found)) {
        free(found);
        return 0;
    }
    free((void *) c->serial.device);
    c->serial.device = found;
    return 1;
}
