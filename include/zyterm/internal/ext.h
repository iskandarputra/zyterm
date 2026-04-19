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

/* ── ext/loglevel.c ────────────────────────────────────────────────────── */
bool loglevel_muted(const zt_ctx *c, const unsigned char *line, size_t len);

/* ── ext/multi.c ───────────────────────────────────────────────────────── */
int  multi_start(zt_ctx *primary, const char **extra_devices, int n);
void multi_stop(zt_ctx *c);
void multi_tick(zt_ctx *c);
void multi_render(zt_ctx *c);
void multi_embed_reset(void);

/* ── ext/profile.c ─────────────────────────────────────────────────────── */
int profile_load(zt_ctx *c, const char *name);
int profile_save(zt_ctx *c, const char *name);

/* ── ext/reconnect.c ───────────────────────────────────────────────────── */
void run_reconnect_loop(zt_ctx *c);

#endif /* ZYTERM_INTERNAL_EXT_H_ */
