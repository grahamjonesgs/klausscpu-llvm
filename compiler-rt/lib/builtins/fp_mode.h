//===----- lib/fp_mode.h - Floaing-point environment mode utilities --C -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is not part of the interface of this library.
//
// This file defines an interface for accessing hardware floating point
// environment mode.
//
//===----------------------------------------------------------------------===//

#ifndef FP_MODE_H
#define FP_MODE_H

typedef enum {
  CRT_FE_TONEAREST,
  CRT_FE_DOWNWARD,
  CRT_FE_UPWARD,
  CRT_FE_TOWARDZERO
} CRT_FE_ROUND_MODE;

#if defined(__klausscpu__)
// KlaussCPU has no floating-point environment: rounding is always to nearest
// and exceptions are not recorded. Give callers inline definitions so that
// the soft-float add/sub routines do not make out-of-line calls on every
// rounding (which also forces them to set up a frame and save registers).
// GNU extern inline only provides a definition for inlining, so an
// out-of-line definition that includes this header (fp_mode.c, or a runtime's
// stub) still compiles and links; it is simply no longer called.
#define CRT_FP_MODE_INLINE                                                     \
  extern __inline__ __attribute__((__gnu_inline__, __always_inline__))
CRT_FP_MODE_INLINE CRT_FE_ROUND_MODE __fe_getround(void) {
  return CRT_FE_TONEAREST;
}
CRT_FP_MODE_INLINE int __fe_raise_inexact(void) { return 0; }
#undef CRT_FP_MODE_INLINE
#else
CRT_FE_ROUND_MODE __fe_getround(void);
int __fe_raise_inexact(void);
#endif

#endif // FP_MODE_H
