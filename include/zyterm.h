/**
 * @file    zyterm.h
 * @brief   Public API for embedding zyterm into another program.
 *
 * zyterm can run as a standalone binary or be linked into a larger host
 * application (for example, the `zy` shell as a builtin). Embedders only
 * need the single entry point below.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#ifndef ZYTERM_PUBLIC_H
#define ZYTERM_PUBLIC_H

#include <setjmp.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief   zyterm CLI entry point.
 *
 * Behaves exactly like @c main for the standalone binary: parses @p argv,
 * runs the requested mode (interactive / dump / replay), and returns an
 * exit status.
 *
 * @param argc Argument count.
 * @param argv Argument vector (argv[0] is the program name).
 * @return Process exit status.
 */
int zyterm_main(int argc, char **argv);

/* ── Embedding hooks ────────────────────────────────────────────────────
 * Set @ref zt_g_embedded to true before calling @ref zyterm_main when
 * running zyterm in-process inside another program (e.g. the zy shell).
 * That suppresses behaviours that would terminate the host process
 * (alarm watchdog, raise()/_exit() in crash handlers).
 *
 * For full crash-safety, the host should also arm @ref zt_g_embed_jmp
 * via sigsetjmp() before the call. Fatal paths inside zyterm will then
 * siglongjmp back to the host instead of calling exit() / _exit(). Call
 * @ref zt_embed_disarm() after a normal return.
 */
extern bool       zt_g_embedded;
extern sigjmp_buf zt_g_embed_jmp;
extern bool       zt_g_embed_jmp_armed;
void              zt_embed_disarm(void);

/**
 * @brief Append a one-line trace record to ZYTERM_TRACE if set.
 *
 * No-op when the env var is unset. Used to diagnose embedded-mode
 * fatal paths (zt_die, sig_crash, signal install/uninstall) without
 * requiring strace or a debugger attach.
 */
void zt_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * @brief Reset zyterm's process-wide state to a clean slate.
 *
 * The host MUST call this immediately before each in-process
 * @ref zyterm_main invocation. It restores any signal handlers zyterm
 * left installed, clears the quit/winch flags, drops the cached stdin
 * snapshot, and zeroes the output buffer. This guarantees the second
 * (and every subsequent) embedded run starts identically to the first,
 * regardless of how the previous one terminated (normal exit,
 * @c siglongjmp from @ref zt_die, or a crash).
 */
void zt_embed_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ZYTERM_PUBLIC_H */
