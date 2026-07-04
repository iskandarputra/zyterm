/**
 * @file profile.c
 * @brief Named config profiles stored as tiny INI files.
 *
 * Profiles live in `$XDG_CONFIG_HOME/zyterm/<name>.conf`, falling back
 * to `~/.config/zyterm/<name>.conf`. Only a handful of well-known keys
 * are recognized:
 *
 *   device       = /dev/ttyUSB0
 *   baud         = 115200
 *   data_bits    = 8
 *   parity       = N
 *   stop_bits    = 1
 *   flow         = none
 *   frame        = raw|cobs|slip|hdlc|lenpfx
 *   crc          = none|ccitt|ibm|crc32
 *   log_format   = text|json|raw
 *   reconnect    = true|false
 *   osc52        = true|false
 *   map_out      = none|cr|lf|crlf|cr_crlf|lf_crlf
 *   map_in       = none|cr|lf|crlf|cr_crlf|lf_crlf
 *   watch_beep   = true|false
 *   watch        = <regex>          (repeatable, up to ZT_WATCH_MAX)
 *   macro<n>     = <string>         (n = 1..12, e.g. `macro1 = AT+RST\r`)
 *
 * Unknown keys are ignored on load; save rewrites the full known set.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zyterm/internal/ext.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Resolve "$XDG_CONFIG_HOME/zyterm/<name>.conf" (falling back to
 * "$HOME/.config/zyterm/<name>.conf"). Exposed for ext/profile_watch.c. */
void zt_profile_path(const char *name, char *out, size_t cap) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        snprintf(out, cap, "%s/zyterm/%s.conf", xdg, name);
    else
        snprintf(out, cap, "%s/.config/zyterm/%s.conf", getenv("HOME"), name);
}

static void profile_path(const char *name, char *out, size_t cap) {
    zt_profile_path(name, out, cap);
}

static void mkparent(const char *path) {
    char tmp[512];
    strncpy(tmp, path, sizeof tmp - 1);
    tmp[sizeof tmp - 1] = 0;
    char *slash         = strrchr(tmp, '/');
    if (!slash) return;
    *slash = 0;
    /* make each parent */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    mkdir(tmp, 0700);
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

int profile_load(zt_ctx *c, const char *name) {
    if (!c || !name) return -1;
    char path[512];
    profile_path(name, path, sizeof path);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    /* Watches are repeatable, so the first `watch =` line of a load pass clears
     * any existing set before appending — otherwise a profile-watch re-load
     * (or a second profile_load) would accumulate duplicates. */
    bool watch_cleared = false;
    char line[512];
    while (fgets(line, sizeof line, fp)) {
        char *p = trim(line);
        if (!*p || *p == '#' || *p == ';') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq     = 0;
        char *k = trim(p), *v = trim(eq + 1);
        if (!strcmp(k, "device")) {
            free((void *)c->serial.device);
            c->serial.device = strdup(v);
        } else if (!strcmp(k, "baud"))
            c->serial.baud = (unsigned)atoi(v);
        else if (!strcmp(k, "data_bits"))
            c->serial.data_bits = atoi(v);
        else if (!strcmp(k, "parity"))
            c->serial.parity = v[0];
        else if (!strcmp(k, "stop_bits"))
            c->serial.stop_bits = atoi(v);
        else if (!strcmp(k, "flow"))
            c->serial.flow = !strcmp(v, "rtscts") ? 1 : !strcmp(v, "xonxoff") ? 2 : 0;
        else if (!strcmp(k, "reconnect"))
            c->core.reconnect = !strcmp(v, "true");
        else if (!strcmp(k, "osc52"))
            c->proto.osc52_enabled = !strcmp(v, "true");
        else if (!strcmp(k, "frame")) {
            if (!strcmp(v, "cobs"))
                c->proto.mode = ZT_FRAME_COBS;
            else if (!strcmp(v, "slip"))
                c->proto.mode = ZT_FRAME_SLIP;
            else if (!strcmp(v, "hdlc"))
                c->proto.mode = ZT_FRAME_HDLC;
            else if (!strcmp(v, "lenpfx"))
                c->proto.mode = ZT_FRAME_LENPFX;
            else
                c->proto.mode = ZT_FRAME_RAW;
        } else if (!strcmp(k, "crc")) {
            if (!strcmp(v, "ccitt"))
                c->proto.crc_mode = ZT_CRC_CCITT;
            else if (!strcmp(v, "ibm"))
                c->proto.crc_mode = ZT_CRC_IBM;
            else if (!strcmp(v, "crc32"))
                c->proto.crc_mode = ZT_CRC_CRC32;
            else
                c->proto.crc_mode = ZT_CRC_NONE;
        } else if (!strcmp(k, "log_format")) {
            if (!strcmp(v, "json"))
                c->log.format = ZT_LOG_JSON;
            else if (!strcmp(v, "raw"))
                c->log.format = ZT_LOG_RAW;
            else
                c->log.format = ZT_LOG_TEXT;
        } else if (!strcmp(k, "map_out")) {
            (void)eol_parse(v, &c->proto.map_out);
        } else if (!strcmp(k, "map_in")) {
            (void)eol_parse(v, &c->proto.map_in);
        } else if (!strcmp(k, "watch_beep")) {
            c->log.watch_beep = !strcmp(v, "true");
        } else if (!strcmp(k, "watch")) {
            if (!watch_cleared) {
                for (int i = 0; i < c->log.watch_count; i++) {
                    free(c->log.watch[i]);
                    c->log.watch[i] = NULL;
                }
                c->log.watch_count = 0;
                watch_cleared      = true;
            }
            if (*v && c->log.watch_count < ZT_WATCH_MAX)
                c->log.watch[c->log.watch_count++] = strdup(v);
        } else if (!strncmp(k, "macro", 5) && isdigit((unsigned char)k[5])) {
            int n = atoi(k + 5);
            if (n >= 1 && n <= ZT_MACRO_COUNT) {
                free(c->ext.macros[n - 1]);
                c->ext.macros[n - 1] = strdup(v);
            }
        }
    }
    fclose(fp);
    log_notice(c, "profile loaded: %s", name);
    return 0;
}

int profile_save(zt_ctx *c, const char *name) {
    if (!c || !name) return -1;
    char path[512];
    profile_path(name, path, sizeof path);
    mkparent(path);
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "# zyterm profile: %s\n", name);
    if (c->serial.device) fprintf(fp, "device = %s\n", c->serial.device);
    fprintf(fp, "baud = %u\n", c->serial.baud);
    fprintf(fp, "data_bits = %d\n", c->serial.data_bits);
    fprintf(fp, "parity = %c\n", c->serial.parity);
    fprintf(fp, "stop_bits = %d\n", c->serial.stop_bits);
    fprintf(fp, "flow = %s\n",
            c->serial.flow == 1   ? "rtscts"
            : c->serial.flow == 2 ? "xonxoff"
                                  : "none");
    fprintf(fp, "reconnect = %s\n", c->core.reconnect ? "true" : "false");
    fprintf(fp, "osc52 = %s\n", c->proto.osc52_enabled ? "true" : "false");
    static const char *FM[] = {"raw", "cobs", "slip", "hdlc", "lenpfx"};
    static const char *CM[] = {"none", "ccitt", "ibm", "crc32"};
    static const char *LF[] = {"text", "json", "raw"};
    fprintf(fp, "frame = %s\n", FM[c->proto.mode]);
    fprintf(fp, "crc = %s\n", CM[c->proto.crc_mode]);
    fprintf(fp, "log_format = %s\n", LF[c->log.format]);
    fprintf(fp, "map_out = %s\n", eol_name(c->proto.map_out));
    fprintf(fp, "map_in = %s\n", eol_name(c->proto.map_in));
    fprintf(fp, "watch_beep = %s\n", c->log.watch_beep ? "true" : "false");
    /* --watch patterns (repeatable) and F1..F12 macros. Both are single-line
     * (macros store literal \r/\n/\xNN escapes, resolved at fire time), so they
     * round-trip through the INI value verbatim. */
    for (int i = 0; i < c->log.watch_count; i++)
        if (c->log.watch[i]) fprintf(fp, "watch = %s\n", c->log.watch[i]);
    for (int i = 0; i < ZT_MACRO_COUNT; i++)
        if (c->ext.macros[i]) fprintf(fp, "macro%d = %s\n", i + 1, c->ext.macros[i]);
    fclose(fp);
    log_notice(c, "profile saved: %s", name);
    return 0;
}
