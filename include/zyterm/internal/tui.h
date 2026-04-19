/**
 * @file    tui.h
 * @brief   Terminal UI — HUD, input bar, dialogs, search/rename overlays,
 *          less-style pager, fuzzy finder.
 *
 * Module: @c tui/.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#ifndef ZYTERM_INTERNAL_TUI_H_
#define ZYTERM_INTERNAL_TUI_H_

#include "render.h"

/* ── tui/hud.c ─────────────────────────────────────────────────────────── */
void   fmt_bytes(uint64_t v, char *out, size_t cap);
void   fmt_hms(double sec, char *out, size_t cap);
void   query_winsize(zt_ctx *c);
void   draw_hud(zt_ctx *c);
size_t build_prompt(zt_ctx *c, char *buf, size_t cap);
void   draw_input(zt_ctx *c);
void   apply_layout(zt_ctx *c);
void   draw_dialog(zt_ctx *c, const char *title_icon, const char *title, const char *accent_fg,
                   const char *const *body, int body_n, const char *footer);
void   draw_cmd_popup(zt_ctx *c);
void   draw_keybind_popup(zt_ctx *c);
void   draw_settings_page(zt_ctx *c);
void   draw_disconnect_popup(zt_ctx *c, int dots);
void   draw_search_bar(zt_ctx *c);
void   draw_rename_bar(zt_ctx *c);
int    search_scrollback(zt_ctx *c, int dir);
void   set_flash(zt_ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ── tui/pager.c ───────────────────────────────────────────────────────── */
bool pager_handle(zt_ctx *c, unsigned char k);

/* ── tui/fuzzy.c ───────────────────────────────────────────────────────── */
void fuzzy_enter(zt_ctx *c);
void fuzzy_exit(zt_ctx *c);
bool fuzzy_handle(zt_ctx *c, unsigned char k);
void fuzzy_draw(zt_ctx *c);

#endif /* ZYTERM_INTERNAL_TUI_H_ */
