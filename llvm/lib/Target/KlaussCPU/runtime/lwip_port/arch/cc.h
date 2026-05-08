// arch/cc.h — lwIP compiler/architecture bridge for KlaussCPU.
//
// KlaussCPU type model (matches KlaussCPUTargetInfo and DataLayout):
//   char=8  short=16  int=32  long=64  long long=64  pointer=64
// Memory: little-endian.
// Compiler: clang with picolibc.

#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>
#include <limits.h>
/* picolibc's limits.h is bare-metal and omits SSIZE_MAX (a POSIX extension).
 * arch.h checks #ifdef SSIZE_MAX to decide whether to 'typedef int ssize_t'.
 * Define it here (cc.h is included first, at arch.h line 48) so lwIP takes
 * the #ifdef branch and we skip the conflicting typedef. */
#ifndef SSIZE_MAX
#define SSIZE_MAX  LONG_MAX   /* ssize_t = long = 64-bit on KlaussCPU */
#endif
#include <stdio.h>

// ── Scalar types ──────────────────────────────────────────────────────────

typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;   // unsigned int  = 32-bit
typedef int32_t   s32_t;   // signed int    = 32-bit
typedef uint64_t  u64_t;   // unsigned long = 64-bit
typedef int64_t   s64_t;

typedef unsigned long mem_ptr_t;   // pointer-sized (64-bit on KlaussCPU)

// ── printf format specifiers ──────────────────────────────────────────────
// picolibc does not support the 'll' length modifier; long = 64-bit here.
// u32_t = unsigned int (32-bit) → %u.  size_t = unsigned long → %lu.

#define U16_F   "u"
#define S16_F   "d"
#define X16_F   "x"
#define U32_F   "u"
#define S32_F   "d"
#define X32_F   "x"
#define SZT_F   "lu"    // size_t = unsigned long = 64-bit

// ── Endianness ────────────────────────────────────────────────────────────

#define BYTE_ORDER   LITTLE_ENDIAN

// ── Struct packing ────────────────────────────────────────────────────────

#define PACK_STRUCT_STRUCT  __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

// ── Diagnostics ───────────────────────────────────────────────────────────

#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) \
    do { printf("lwIP ASSERT %s:%d: %s\n", __FILE__, __LINE__, (x)); \
         while (1) {} } while (0)

// ── Compiler hints ────────────────────────────────────────────────────────

#define LWIP_UNUSED_ARG(x)  (void)(x)

#endif // LWIP_ARCH_CC_H
