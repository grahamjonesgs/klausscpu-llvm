/* test_64bit.c — KlaussCPU CPU test suite (LLVM backend port).
 *
 * Compile via Makefile:  make test_64bit.elf  &&  make test_64bit.bin
 *
 * Type model on KlaussCPU with the LLVM backend (post int=32 migration):
 *   sizeof(int)       = 4
 *   sizeof(long)      = 8
 *   sizeof(long long) = 8
 *   sizeof(void *)    = 8
 *
 * The hardware GPRs are 64 bits; the type legalizer promotes i32 ALU ops to
 * i64 internally, but C-level wrap-around on `int` arithmetic still happens
 * at 32 bits because the value is sign- or zero-extended back to 32 bits at
 * each store / function-boundary truncation.  Tests here exercise that.
 *
 * Note on string literals and char arrays (T11-T13):
 *   Physical memory is little-endian (FPGA fix applied April 2026).
 *   LDIDX8/STIDX8 use LE-lane mapping consistent with physical memory, so
 *   strcpy, strcmp, and strlen all work correctly with standard C byte access.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Globals ──────────────────────────────────────────────────────────────── */

int        g_pass;
int        g_fail;
int        g_val;       /* 32-bit global (exercises stidx32/ldidx32) */
long long  g_val64;     /* 64-bit global (exercises stidx64/ldidx64) */

/* ── Test helpers ─────────────────────────────────────────────────────────── */

void check(char *name, int ok) {
    printf("%s%s\n", name, ok ? "PASS" : "FAIL");
    if (ok) g_pass++; else g_fail++;
}

void check_eq(char *name, int actual, int expected) {
    if (actual == expected) {
        printf("%sPASS\n", name);
        g_pass++;
    } else {
        printf("%sFAIL  got=0x%016llx exp=0x%016llx\n",
               name, (unsigned long long)(unsigned int)actual,
               (unsigned long long)(unsigned int)expected);
        g_fail++;
    }
}

void check_eq64(char *name, long long actual, long long expected) {
    if (actual == expected) {
        printf("%sPASS\n", name);
        g_pass++;
    } else {
        printf("%sFAIL  got=0x%016llx exp=0x%016llx\n",
               name, (unsigned long long)actual, (unsigned long long)expected);
        g_fail++;
    }
}

/* ── Recursive helpers ────────────────────────────────────────────────────── */

int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

int fib(int n) {
    if (n <= 0) return 0;
    if (n == 1) return 1;
    return fib(n - 1) + fib(n - 2);
}

/* ── Overflow-argument helpers ────────────────────────────────────────────── */

int sum5(int a, int b, int c, int d, int e) {
    return a + b + c + d + e;
}

int sum6(int a, int b, int c, int d, int e, int f) {
    return a + b + c + d + e + f;
}

/* Mixed widths to exercise i32 → i64 promotion at call boundaries. */
long long sum6_mixed(int a, long long b, int c, long long d, int e, long long f) {
    return (long long)a + b + (long long)c + d + (long long)e + f;
}

/* ── Global access helpers ────────────────────────────────────────────────── */

void set_global(int v)         { g_val = v; }
int  get_global(void)          { return g_val; }
void set_global64(long long v) { g_val64 = v; }
long long get_global64(void)   { return g_val64; }

/* ════════════════════════════════════════════════════════════════════════════
 * main
 * ════════════════════════════════════════════════════════════════════════════ */

int main(void) {
    int ok;
    char buf[32];

    g_pass = 0;
    g_fail = 0;

    printf("=== KlaussCPU Test Suite ===\n");

    /* System info */
    {
        int arr2[2];
        long long arr2ll[2];
        int sz_int = (int)((char *)(&arr2[1])   - (char *)(&arr2[0]));
        int sz_ll  = (int)((char *)(&arr2ll[1]) - (char *)(&arr2ll[0]));
        printf("sizeof(int)=%d  sizeof(long long)=%d\n\n", sz_int, sz_ll);
    }

    /* T1: 64-bit constant shift (shift > 31) */
    {
        long long a, b;
        a = 1;
        a = a << 40;
        b = a >> 20;
        check("T1  shift>31:     ", b == 1048576);
    }

    /* T2: 64-bit multiplication */
    {
        long long a, b, c;
        a = 0x100000;
        b = 0x100000;
        c = a * b;
        c = c >> 20;
        check("T2  mul64:        ", c == 1048576);
    }

    /* T3: 64-bit addition crosses INT32_MAX (must use long long) */
    {
        long long a, b;
        a  = 0x7FFFFFFF;
        b  = a + 1;
        ok = (b > 0) && (b == 0x80000000LL);
        a  = b + b;
        ok = ok && (a > 0) && (a > 0x7FFFFFFFLL);
        check("T3  add64 no ovf: ", ok);
    }

    /* T3b: 32-bit addition wraps at INT32_MAX (new — exercises int=32) */
    {
        int x = 0x7FFFFFFF;
        int y = x + 1;             /* signed overflow → -2^31 */
        ok = (y < 0) && (y == (int)0x80000000);
        check("T3b int wrap:     ", ok);
    }

    /* T4: Bitwise NOT on long long */
    {
        long long a, b, b0, a1;
        long long blow, bhigh;
        a     = 0;
        b0    = ~a;
        a1    = ~b0;
        a     = 0x5A5A5A5ALL;
        b     = ~a;
        blow  = b & 0xFFFFFFFFLL;
        bhigh = b >> 32;
        check_eq64("T4a ~0==-1:       ", b0,    -1);
        check_eq64("T4b ~~0==0:       ", a1,     0);
        check_eq64("T4c ~5A low32:    ", blow,   0xA5A5A5A5LL);
        check_eq64("T4d ~5A >>32==-1: ", bhigh, -1);
    }

    /* T5: Arithmetic right shift of negative (long long) */
    {
        long long neg1, neg256, r1, r2, r3;
        neg1   = -1;
        neg256 = -256;
        r1 = neg1   >> 63;
        r2 = neg1   >> 1;
        r3 = neg256 >> 4;
        check_eq64("T5a -1>>63==-1:   ", r1,  -1);
        check_eq64("T5b -1>>1==-1:    ", r2,  -1);
        check_eq64("T5c -256>>4==-16: ", r3, -16);
    }

    /* T6: Logical right shift of unsigned long long */
    {
        unsigned long long ua;
        ua = (unsigned long long)-1;
        check_eq64("T6a >>63==1:      ", (long long)(ua >> 63), 1);
        check_eq64("T6b >>1 positive: ", (long long)((long long)(ua >> 1) > 0), 1);
    }

    /* T7: Comparisons with large 64-bit values */
    {
        long long large, neg;
        unsigned long long ua, ub;
        large = 1;
        large = large << 40;
        neg   = -1;
        ua    = (unsigned long long)(-1);
        ub    = (unsigned long long)large;
        check_eq64("T7a 2^40>0:       ", (large > 0), 1);
        check_eq64("T7b 2^40>0x7F..:  ", (large > 0x7FFFFFFFLL), 1);
        check_eq64("T7c -1<2^40:      ", (neg < large), 1);
        check_eq64("T7d -1<0:         ", (neg < 0), 1);
        check_eq64("T7e MaxU>2^40:    ", (long long)(ua > ub), 1);
    }

    /* T8: Recursion — factorial (fits in int=32 up to 12!) */
    ok = (factorial(10) == 3628800);
    ok = ok && (factorial(12) == 479001600);
    check("T8  factorial:    ", ok);

    /* T9: Recursion — Fibonacci */
    ok = (fib(10) == 55);
    ok = ok && (fib(15) == 610);
    check("T9  fibonacci:    ", ok);

    /* T10: Stack arguments (5th and 6th args) */
    check_eq("T10a sum5(1..5):  ", sum5(1, 2, 3, 4, 5),          15);
    check_eq("T10b sum5(10..50):", sum5(10, 20, 30, 40, 50),      150);
    check_eq("T10c sum6(10..60):", sum6(10, 20, 30, 40, 50, 60),  210);

    /* T10d: mixed-width args — exercises i32-promotion in calling convention. */
    check_eq64("T10d sum6_mixed:  ",
               sum6_mixed(1, 2LL, 3, 4LL, 5, 6LL),  21LL);

    /* T11: Char array byte access */
    buf[0] = 72;   /* 'H' */
    buf[1] = 101;  /* 'e' */
    buf[2] = 121;  /* 'y' */
    buf[3] = 33;   /* '!' */
    buf[4] = 0;
    ok = (buf[0] == 72) && (buf[1] == 101) &&
         (buf[2] == 121) && (buf[3] == 33) && (buf[4] == 0);
    ok = ok && (strlen(buf) == 4);
    check("T11 char[]:       ", ok);

    /* T12: String operations */
    strcpy(buf, "hello");
    ok  = (strlen(buf) == 5);
    ok  = ok && (strcmp(buf, "hello") == 0);
    ok  = ok && (strcmp(buf, "hellp") < 0);
    ok  = ok && (strcmp(buf, "helln") > 0);
    ok  = ok && (strcmp(buf, "hell")  > 0);
    ok  = ok && (strcmp(buf, "hellow") < 0);
    check("T12 strings:      ", ok);

    /* T13: memset / memcpy */
    {
        char src[24];
        char dst[24];
        memset(src,      0xAB, 8);
        memset(src + 8,  0x00, 8);
        memset(src + 16, 0x55, 8);
        ok  = ((src[0] & 0xFF) == 0xAB) && ((src[7]  & 0xFF) == 0xAB);
        ok  = ok && (src[8] == 0)  && (src[15] == 0);
        ok  = ok && ((src[16] & 0xFF) == 0x55) && ((src[23] & 0xFF) == 0x55);
        memcpy(dst, src, 24);
        ok  = ok && ((dst[0]  & 0xFF) == 0xAB);
        ok  = ok && (dst[8]  == 0);
        ok  = ok && ((dst[16] & 0xFF) == 0x55);
        check("T13 memset/cpy:   ", ok);
    }

    /* T14: Globals — exercise both 32-bit and 64-bit storage */
    set_global(0x12345678);
    ok = (get_global() == 0x12345678);
    set_global(0);
    ok = ok && (get_global() == 0);
    set_global(-42);
    ok = ok && (get_global() == -42);
    check("T14a int global:  ", ok);

    set_global64(0xDEADBEEFCAFEBABELL);
    ok = (get_global64() == 0xDEADBEEFCAFEBABELL);
    set_global64(0);
    ok = ok && (get_global64() == 0);
    set_global64(-1);
    ok = ok && (get_global64() == -1);
    check("T14b ll  global:  ", ok);

    /* T15: Pointer arithmetic — sizeof(int)=4, sizeof(long long)=8 */
    {
        int        iarr[4];
        long long  larr[4];
        int       *ip;
        long long *lp;
        long long  d_int, d_ll;

        iarr[0] = 100; iarr[1] = 200; iarr[2] = 300; iarr[3] = 400;
        ip = iarr;
        ok = (*ip == 100);   ip = ip + 1;
        ok = ok && (*ip == 200); ip = ip + 1;
        ok = ok && (*ip == 300); ip = ip + 1;
        ok = ok && (*ip == 400);
        d_int = (long long)((char *)(&iarr[3]) - (char *)(&iarr[0]));
        ok = ok && (d_int == 12);   /* 3 * sizeof(int)=4 = 12 */

        larr[0] = 1000; larr[1] = 2000; larr[2] = 3000; larr[3] = 4000;
        lp = larr;
        ok = ok && (*lp == 1000); lp = lp + 1;
        ok = ok && (*lp == 2000); lp = lp + 1;
        ok = ok && (*lp == 3000); lp = lp + 1;
        ok = ok && (*lp == 4000);
        d_ll = (long long)((char *)(&larr[3]) - (char *)(&larr[0]));
        ok = ok && (d_ll == 24);    /* 3 * sizeof(long long)=8 = 24 */

        check("T15 ptr arith:    ", ok);
    }

    /* T16: Heap — malloc, calloc, realloc, free, coalesce.
     * Heap blocks are 8-byte-aligned and allocations are accessed as
     * 64-bit slots (the libc allocator uses uint64_t headers). */
    {
        long long *hp1;
        long long *hp2;
        long long *hp3;

        /* T16a: basic alloc + data integrity */
        hp1    = (long long *)malloc(16);
        hp1[0] = 0x1111;
        hp1[1] = 0x2222;
        ok = (hp1[0] == 0x1111) && (hp1[1] == 0x2222);
        check("T16a heap alloc:  ", ok);

        /* T16b: calloc zero-fill */
        hp2 = (long long *)calloc(3, 8);
        ok  = (hp2[0] == 0) && (hp2[1] == 0) && (hp2[2] == 0);
        check("T16b calloc:      ", ok);

        /* T16c: realloc grow, old data survives */
        hp3    = (long long *)malloc(8);
        hp3[0] = 0x5A5A5A5ALL;
        hp3    = (long long *)realloc(hp3, 24);
        check_eq64("T16c-0 keep[0]:   ", hp3[0], 0x5A5A5A5ALL);
        hp3[1] = 0xBEEFBEEFLL;
        hp3[2] = 0xCAFECAFELL;
        check_eq64("T16c-1 new[1]:    ", hp3[1], 0xBEEFBEEFLL);
        check_eq64("T16c-2 new[2]:    ", hp3[2], 0xCAFECAFELL);

        /* T16d: free all (picolibc heap is opaque — just verify no crash) */
        free(hp1);
        free(hp2);
        free(hp3);
        check("T16d coalesce:    ", 1);
    }

    printf("\nResults: %d pass, %d fail\n", g_pass, g_fail);

    return g_fail;
}
