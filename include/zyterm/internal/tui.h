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
/* set_flash() moved to core.h (cross-cutting notification primitive). */

/* ── tui/scrollback_view.c ─────────────────────────────────────────────── */
/* Viewport over the log/scrollback.c line ring: draw, scroll, and mouse-driven
 * text selection → OSC 52 copy. In the tui layer because it reaches down into
 * render (emit_colored_line) and proto (osc52_copy). */
void redraw_scrollback(zt_ctx *c);
void scroll_up(zt_ctx *c, int lines);
void scroll_down(zt_ctx *c, int lines);
void leave_scroll(zt_ctx *c);
/* In-app text selection (mouse-driven). All coordinates are 1-based screen
 * cells; row must be inside the body region (2..rows-1). */
void selection_begin(zt_ctx *c, int row, int col);
void selection_extend(zt_ctx *c, int row, int col);
void selection_finish(zt_ctx *c); /**< Release -> build text + OSC 52 copy. */
void selection_clear(zt_ctx *c);
void selection_copy(zt_ctx *c); /**< Re-copy current selection (right-click). */

/* ── tui/pager.c ───────────────────────────────────────────────────────── */
bool pager_handle(zt_ctx *c, unsigned char k);

/* ── tui/fuzzy.c ───────────────────────────────────────────────────────── */
void fuzzy_enter(zt_ctx *c);
void fuzzy_exit(zt_ctx *c);
bool fuzzy_handle(zt_ctx *c, unsigned char k);
void fuzzy_draw(zt_ctx *c);

#endif /* ZYTERM_INTERNAL_TUI_H_ */
