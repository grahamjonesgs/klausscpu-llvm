/* test_asm.c — Inline assembly smoke test for KlaussCPU.
 *
 * Compile via Makefile:  make test_asm.elf  &&  make test_asm.bin
 *
 * Exercises the KlaussCPU AsmParser (KlaussCPUAsmParser.cpp) and the
 * inline-asm constraint plumbing added in step 31:
 *
 *   T1  — input/output register constraints  ("=r" / "r")
 *   T2  — multi-operand ALU via inline asm
 *   T3  — volatile asm with side effects (leds, delayv)
 *   T4  — immediate operand in asm template
 *   T5  — clobber list
 *   T6  — chained asm building a value step by step
 *   T7  — asm inside a loop
 */

#include <stdio.h>

static int g_pass;
static int g_fail;

static void check(char *name, int ok) {
    printf("%s%s\n", name, ok ? "PASS" : "FAIL");
    if (ok) g_pass++; else g_fail++;
}

/* ── T1: basic input/output register constraint ──────────────────────────── */

static long long asm_add(long long a, long long b) {
    long long r;
    __asm__("addr %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

static long long asm_sub(long long a, long long b) {
    long long r;
    __asm__("subr %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

/* ── T2: multi-operand ALU ───────────────────────────────────────────────── */

static long long asm_and(long long a, long long b) {
    long long r;
    __asm__("andr %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

static long long asm_or(long long a, long long b) {
    long long r;
    __asm__("orr %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

static long long asm_xor(long long a, long long b) {
    long long r;
    __asm__("xorr %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

static long long asm_shl(long long a, long long b) {
    long long r;
    __asm__("shlr %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

static long long asm_shr(long long a, long long b) {
    long long r;
    __asm__("shrr %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

/* ── T3: volatile asm with hardware side effects ─────────────────────────── */

static void asm_leds(long long pattern) {
    __asm__ volatile("ledr %0" : : "r"(pattern));
}

/* ── T4: in-place (tied) constraint ─────────────────────────────────────── */

static long long asm_negate(long long x) {
    /* notr: bitwise NOT in-place */
    long long r = x;
    __asm__("notr %0" : "+r"(r));
    return r;
}

/* ── T5: clobber list ────────────────────────────────────────────────────── */

static long long asm_mul_clobber(long long a, long long b) {
    long long r;
    __asm__("mulr %0, %1, %2"
            : "=r"(r)
            : "r"(a), "r"(b)
            : /* no extra clobbers beyond output */);
    return r;
}

/* ── T6: chained asm — build a value step by step ───────────────────────── */

static long long asm_chain(long long x) {
    /* Compute ((x + 1) * 2) - 1  using three separate asm blocks. */
    long long step1, step2, step3;
    long long one = 1, two = 2;
    __asm__("addr %0, %1, %2" : "=r"(step1) : "r"(x),     "r"(one));
    __asm__("mulr %0, %1, %2" : "=r"(step2) : "r"(step1), "r"(two));
    __asm__("subr %0, %1, %2" : "=r"(step3) : "r"(step2), "r"(one));
    return step3;
}

/* ── T7: asm inside a loop ───────────────────────────────────────────────── */

static long long asm_sum_loop(long long n) {
    /* Sum 1..n using asm add inside the loop. */
    long long total = 0;
    for (long long i = 1; i <= n; i++) {
        long long t;
        __asm__("addr %0, %1, %2" : "=r"(t) : "r"(total), "r"(i));
        total = t;
    }
    return total;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    g_pass = 0;
    g_fail = 0;

    printf("=== Inline Assembly Test ===\n");

    /* Signal entry via LEDs so hardware bring-up is visible. */
    asm_leds(0x1234);

    /* T1: basic register constraints */
    {
        int ok = 1;
        ok = ok && (asm_add(3, 4)   == 7);
        ok = ok && (asm_add(-1, 1)  == 0);
        ok = ok && (asm_add(0, 0)   == 0);
        ok = ok && (asm_sub(10, 3)  == 7);
        ok = ok && (asm_sub(0, 5)   == -5);
        check("T1 add/sub constraints: ", ok);
    }

    /* T2: multi-operand ALU */
    {
        int ok = 1;
        ok = ok && (asm_and(0xFF, 0x0F)  == 0x0F);
        ok = ok && (asm_and(0, 0xFF)     == 0);
        ok = ok && (asm_or (0xF0, 0x0F)  == 0xFF);
        ok = ok && (asm_xor(0xFF, 0x0F)  == 0xF0);
        ok = ok && (asm_shl(1, 4)        == 16);
        ok = ok && (asm_shr(256, 3)      == 32);
        check("T2 multi-op ALU:        ", ok);
    }

    /* T3: volatile asm side effects (just verify it doesn't crash) */
    {
        asm_leds(0xAAAA);
        asm_leds(0x5555);
        asm_leds(0x1234);  /* restore */
        check("T3 volatile asm leds:   ", 1);
    }

    /* T4: in-place tied constraint */
    {
        int ok = 1;
        ok = ok && (asm_negate(0)           == -1LL);
        ok = ok && (asm_negate(-1LL)        == 0);
        ok = ok && (asm_negate(0x0F0F0F0F)  == (long long)0xFFFFFFFFF0F0F0F0LL);
        check("T4 in-place (notr):     ", ok);
    }

    /* T5: multiply with clobber list */
    {
        int ok = 1;
        ok = ok && (asm_mul_clobber(6, 7)   == 42);
        ok = ok && (asm_mul_clobber(-3, 4)  == -12);
        ok = ok && (asm_mul_clobber(0, 99)  == 0);
        check("T5 mul + clobber:       ", ok);
    }

    /* T6: chained asm blocks */
    {
        /* ((x+1)*2)-1: x=4 → 5*2-1=9; x=0 → 1*2-1=1; x=-1 → 0*2-1=-1 */
        int ok = 1;
        ok = ok && (asm_chain(4)  == 9);
        ok = ok && (asm_chain(0)  == 1);
        ok = ok && (asm_chain(-1) == -1);
        check("T6 chained asm blocks:  ", ok);
    }

    /* T7: asm inside loop */
    {
        /* sum 1..10 = 55; sum 1..1 = 1; sum 1..0 = 0 */
        int ok = 1;
        ok = ok && (asm_sum_loop(10) == 55);
        ok = ok && (asm_sum_loop(1)  == 1);
        ok = ok && (asm_sum_loop(0)  == 0);
        check("T7 asm in loop:         ", ok);
    }

    printf("\nResults: %d pass, %d fail\n", g_pass, g_fail);

    asm_leds(g_fail == 0 ? 0xFFFF : 0xDEAD);

    return g_fail;
}
