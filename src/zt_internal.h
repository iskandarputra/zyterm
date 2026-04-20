/**
 * @file    zt_internal.h
 * @brief   Compatibility umbrella — pulls in every internal module header.
 *
 * After the modular refactor, declarations live under
 * @c include/zyterm/internal/{core,serial,log,proto,render,tui,net,ext,loop}.h.
 *
 * Each translation unit may include only the module(s) it needs; for source
 * compatibility this header still exposes the full internal API.
 *
 * Module include chain (each header pulls in the one above it):
 *
 *   core   → serial → log → proto → render → tui → net → ext → loop
 *
 * @see docs/ARCHITECTURE.md
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#ifndef ZYTERM_ZT_INTERNAL_H_
#define ZYTERM_ZT_INTERNAL_H_

#include "zyterm/internal/loop.h" /* transitively pulls every module */

/* ── Portable glibc symbol pinning ──────────────────────────────────────────
 *
 * GCC >= 13 on glibc >= 2.38 silently rewrites strtol() / strtoul() to
 * __isoc23_strtol / __isoc23_strtoul (C23 variants) at the *header* level
 * via __REDIRECT_NTH.  This stamps the binary with a GLIBC_2.38 floor,
 * so a zyterm built on Ubuntu 24.04 won't start on Ubuntu 22.04
 * ("version GLIBC_2.38 not found").
 *
 * Since the redirect already happened in <stdlib.h>, we target the
 * redirected names and map them back to the classic GLIBC_2.2.5 symbols.
 * Behaviour is identical for our use-cases (decimal baud-rate + 2-char hex).
 * Has no effect on non-glibc systems (musl, macOS, FreeBSD).
 */
#if defined(__linux__) && defined(__GLIBC__) && defined(__GLIBC_MINOR__)
#if (__GLIBC__ > 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 38)
__asm__(".symver __isoc23_strtol,strtol@GLIBC_2.2.5");
__asm__(".symver __isoc23_strtoul,strtoul@GLIBC_2.2.5");
#endif
#endif

#endif /* ZYTERM_ZT_INTERNAL_H_ */
