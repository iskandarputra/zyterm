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
 *
 * NOTE: skip this under AddressSanitizer / ThreadSanitizer / MemorySanitizer
 * — those install libc interceptors keyed on the modern @GLIBC_2.38 symbol,
 * and our .symver back to GLIBC_2.2.5 bypasses them. The bypass turns a
 * harmless atoi("230400") into a SEGV inside strtol because the interceptor
 * never installs the correct argument-marshalling thunk. Sanitizer builds
 * are dev-only; they don't need old-glibc portability anyway.
 *
 * Detection: GCC defines __SANITIZE_ADDRESS__ / __SANITIZE_THREAD__ when
 * the matching sanitizer is on. Clang uses __has_feature(...) — but we
 * must guard that with a nested #if so GCC (which does not define
 * __has_feature) never tries to evaluate `__has_feature(X)` directly
 * inside an #if expression (GCC would substitute 0 for the unknown
 * identifier and then complain about `0(X)` syntax).
 */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ZT_SANITIZER_ACTIVE 1
#endif
#if !defined(ZT_SANITIZER_ACTIVE) && defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ZT_SANITIZER_ACTIVE 1
#elif __has_feature(thread_sanitizer)
#define ZT_SANITIZER_ACTIVE 1
#elif __has_feature(memory_sanitizer)
#define ZT_SANITIZER_ACTIVE 1
#endif
#endif

#if defined(__linux__) && defined(__GLIBC__) && defined(__GLIBC_MINOR__)
#if !defined(ZT_SANITIZER_ACTIVE)
#if (__GLIBC__ > 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 38)
__asm__(".symver __isoc23_strtol,strtol@GLIBC_2.2.5");
__asm__(".symver __isoc23_strtoul,strtoul@GLIBC_2.2.5");
#endif
#endif
#endif

#endif /* ZYTERM_ZT_INTERNAL_H_ */
