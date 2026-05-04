// fp_mode_stub.c — fenv stubs for KlaussCPU.
//
// KlaussCPU has no hardware FP and no floating-point environment support.
// compiler-rt's soft-float routines call these two functions when a result
// is inexact and the rounding mode needs to be queried / an exception raised.
// We always return round-to-nearest-even and ignore the inexact flag.
#include "fp_mode.h"

CRT_FE_ROUND_MODE __fe_getround(void) { return CRT_FE_TONEAREST; }
int __fe_raise_inexact(void) { return 0; }
