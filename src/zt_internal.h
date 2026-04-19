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

#endif /* ZYTERM_ZT_INTERNAL_H_ */
