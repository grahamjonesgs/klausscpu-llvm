// test_varargs.c — va_arg / printf ABI correctness regression test.
//
// Background: a varargs "mis-pack" was suspected after Zephyr LOG_DBG printed
// garbage (e.g. "size 8589934601" = 0x2_00000009) during llext bring-up.
//
// Investigation result (2026-05, see CLAUDE.md "Known issues"): the COMPILER's
// va_arg lowering is correct.  KlaussCPUTargetLowering::LowerVAARG reads a full
// 8-byte va_list slot and advances by 8 for every argument, matching the ABI's
// promote-everything-to-i64 calling convention, at both -O0 and -O1/-Os, for
// single / multiple / mixed-width / loop consumption.  picolibc printf and
// Zephyr's direct cbvprintf are therefore fine.  The garble came only from
// Zephyr's *packaged* logging path (lib/os/cbprintf_packaged.c), which packs
// each arg by its C-type size (sizeof(int)=4 for %d) — an assumption that does
// not match KlaussCPU's uniform 8-byte vararg slots.  That path is cosmetic and
// already silenced in the production ssh_shell (CONFIG_LLEXT_LOG_LEVEL_WRN).
//
// This test guards the compiler path against future regressions: it builds
// distinct 64-bit sentinels (both 32-bit halves non-zero and per-slot unique),
// reads them back through va_arg in the call-crossing loop pattern that a real
// printf uses, and also checks a mixed 32/64-bit single-call shape.  A correct
// compiler prints "ALL PASS".
//
// Note on printf formats: KlaussCPU `long` is 64-bit and picolibc does NOT
// support the `ll` length modifier, so 64-bit values are printed with %ld
// after casting to `long` (see the printf-format note in project memory).
//
// Build:  make test_varargs.elf      (bare-metal; uses <stdarg.h> so it is not
//                                      built as an .llext extension)

int printf(const char *fmt, ...);   // picolibc / kernel-exported

#include <stdarg.h>

typedef long long          i64;
typedef unsigned long long u64;

static int fails = 0;

// Sentinel for slot i: high half = 0xA0000000+i, low half = 0x0B000000+i, so
// both 32-bit halves are non-zero and distinct per slot.  An off-by-one slot
// read, or one that straddles two slots, changes the value visibly.
static u64 sentinel(int i) {
    return ((u64)(0xA0000000u + (unsigned)i) << 32) | (u64)(0x0B000000u + (unsigned)i);
}

// Variadic consumer mirroring vfprintf: reads each argument with va_arg inside
// a loop whose body makes a call (printf on mismatch), so a value load can be
// separated from va_start by a call boundary.  noinline prevents the optimizer
// from keeping the args in registers and bypassing the va_list entirely.
__attribute__((noinline))
static void check(int count, ...) {
    va_list ap;
    va_start(ap, count);
    for (int i = 0; i < count; i++) {
        u64 got  = va_arg(ap, u64);
        u64 want = sentinel(i);
        if (got != want) {
            printf("  FAIL slot %d: got %08x:%08x  want %08x:%08x\n",
                   i,
                   (unsigned)(got >> 32),  (unsigned)(got & 0xffffffffu),
                   (unsigned)(want >> 32), (unsigned)(want & 0xffffffffu));
            fails++;
        }
    }
    va_end(ap);
}

// Mixed 32/64-bit arguments in a single call — the original failing shape.
__attribute__((noinline))
static void mixed(int a, i64 b, int c, i64 d) {
    printf("  mixed: a=%d b=%ld c=%d d=%ld\n", a, (long)b, c, (long)d);
    if (a != 11)            { printf("  FAIL mixed a\n"); fails++; }
    if (b != 2222222222LL)  { printf("  FAIL mixed b\n"); fails++; }
    if (c != 33)            { printf("  FAIL mixed c\n"); fails++; }
    if (d != 4444444444LL)  { printf("  FAIL mixed d\n"); fails++; }
}

int main(void) {
    printf("=== test_varargs ===\n");

    // T1: eight 64-bit args read in a call-crossing loop.
    check(8, sentinel(0), sentinel(1), sentinel(2), sentinel(3),
             sentinel(4), sentinel(5), sentinel(6), sentinel(7));

    // T2: the mixed-width single-call shape that first raised suspicion.
    mixed(11, 2222222222LL, 33, 4444444444LL);

    if (fails == 0)
        printf("=== ALL PASS ===\n");
    else
        printf("=== %d FAIL ===\n", fails);

    return fails;
}
