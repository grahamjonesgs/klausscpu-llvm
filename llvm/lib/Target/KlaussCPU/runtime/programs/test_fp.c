/* test_fp.c — Floating-point smoke test for KlaussCPU.
 *
 * Compile via Makefile:  make test_fp.elf  &&  make test_fp.bin
 *
 * KlaussCPU has no FP hardware, so every `float` operation lowers to a
 * libcall (`__addsf3`, `__mulsf3`, `__floatsisf`, etc.).  Those libcalls
 * are provided by `softfp.c` in this directory.
 *
 * This test exercises only NORMAL values — the soft-FP runtime here does
 * not handle subnormals, infinities, or NaN.  Exact-in-binary values
 * (powers of 2 and simple fractions) are used so we can compare results
 * via integer truncation without worrying about round-off.
 */

#include <stdio.h>

static int g_pass;
static int g_fail;

static void check(char *name, int ok) {
    printf("%s%s\n", name, ok ? "PASS" : "FAIL");
    if (ok) g_pass++; else g_fail++;
}

/* Helper: compare two floats for "close enough" by checking their integer
 * truncation matches.  Adequate for the values we test (all whole numbers). */
static int feq_int(float a, int expected) {
    return (int)a == expected;
}

/* Force the soft-FP path even when the compiler can constant-fold.
 * Marking variables `volatile` defeats compile-time evaluation. */
static float fpass(float x) { volatile float v = x; return v; }

int main(void) {
    g_pass = 0;
    g_fail = 0;

    printf("=== Soft-FP Test ===\n");

    /* T1: int -> float -> int round trip */
    {
        float f1 = fpass((float)42);
        float f2 = fpass((float)-1234567);
        float f3 = fpass((float)0);
        float f4 = fpass((float)16777216);   /* exactly 2^24 */
        check("T1a 42 round trip:        ", feq_int(f1, 42));
        check("T1b -1234567 round trip:  ", feq_int(f2, -1234567));
        check("T1c 0 round trip:         ", feq_int(f3, 0));
        check("T1d 2^24 round trip:      ", feq_int(f4, 16777216));
    }

    /* T2: addition */
    {
        float a = fpass((float)2);
        float b = fpass((float)3);
        float s = a + b;
        check("T2a 2 + 3 == 5:           ", feq_int(s, 5));

        float c = fpass((float)1000000);
        float d = fpass((float)234567);
        float t = c + d;
        check("T2b 1000000 + 234567:     ", feq_int(t, 1234567));

        /* Negative + positive */
        float e = fpass((float)-50);
        float f = fpass((float)80);
        float u = e + f;
        check("T2c -50 + 80 == 30:       ", feq_int(u, 30));

        /* Exact cancellation */
        float g = fpass((float)17);
        float h = fpass((float)-17);
        float v = g + h;
        check("T2d 17 + -17 == 0:        ", feq_int(v, 0));
    }

    /* T3: subtraction */
    {
        float a = fpass((float)10);
        float b = fpass((float)4);
        check("T3a 10 - 4 == 6:          ", feq_int(a - b, 6));
        check("T3b 4 - 10 == -6:         ", feq_int(b - a, -6));
        check("T3c x - x == 0:           ", feq_int(a - a, 0));
    }

    /* T4: multiplication */
    {
        float a = fpass((float)6);
        float b = fpass((float)7);
        check("T4a 6 * 7 == 42:          ", feq_int(a * b, 42));

        float c = fpass((float)-12);
        float d = fpass((float)13);
        check("T4b -12 * 13 == -156:     ", feq_int(c * d, -156));

        float e = fpass((float)1000);
        float f = fpass((float)1000);
        check("T4c 1000 * 1000 == 1e6:   ", feq_int(e * f, 1000000));
    }

    /* T5: division (exact-in-binary cases) */
    {
        float a = fpass((float)100);
        float b = fpass((float)4);
        check("T5a 100 / 4 == 25:        ", feq_int(a / b, 25));

        float c = fpass((float)1024);
        float d = fpass((float)32);
        check("T5b 1024 / 32 == 32:      ", feq_int(c / d, 32));

        /* 1.0 / 0.5 == 2.0 */
        float e = fpass((float)1);
        float h = fpass((float)2);
        float r = e / h;
        check("T5c 1 / 2 == 0 (trunc):   ", feq_int(r, 0));
        check("T5d 1 / 2 < 1:            ", r < e);
        check("T5e 1 / 2 > 0:            ", r > fpass(0.0f));
    }

    /* T6: composite expressions */
    {
        float a = fpass((float)1);
        float b = fpass((float)2);
        float c = fpass((float)3);
        check("T6a (1+2)*3 == 9:         ", feq_int((a + b) * c, 9));

        float d = fpass((float)100);
        float e = fpass((float)10);
        float f = fpass((float)2);
        check("T6b 100/10 + 100/2 == 60: ", feq_int(d/e + d/f, 60));

        /* (a + b) * (a - b) == a^2 - b^2 */
        float x = fpass((float)17);
        float y = fpass((float)5);
        float lhs = (x + y) * (x - y);
        float rhs = x * x - y * y;
        check("T6c (a+b)(a-b)=a^2-b^2:   ", feq_int(lhs - rhs, 0));
    }

    /* T7: comparisons */
    {
        float a = fpass((float)5);
        float b = fpass((float)3);
        float z = fpass(0.0f);
        check("T7a 5 > 3:                ", a > b);
        check("T7b 3 < 5:                ", b < a);
        check("T7c 5 == 5:               ", a == a);
        check("T7d 5 != 3:               ", a != b);
        check("T7e 3 <= 5:               ", b <= a);
        check("T7f 5 >= 3:               ", a >= b);
        check("T7g 5 >= 5:               ", a >= a);
        check("T7h 0 == -0:              ", z == fpass(-0.0f));
    }

    /* T8: negation */
    {
        float a = fpass((float)17);
        check("T8a -17:                  ", feq_int(-a, -17));
        check("T8b --17 == 17:           ", feq_int(-(-a), 17));
        check("T8c -(-0):                ", -fpass(-0.0f) == fpass(0.0f));
    }

    /* T9: simple iteration — compute the sum 1 + 1/2 + 1/4 + ... + 1/256
     * which equals exactly 511/256 = 1.99609375 in binary float. */
    {
        float total = fpass(0.0f);
        float term  = fpass((float)1);
        float half  = fpass((float)1) / fpass((float)2);
        for (int i = 0; i < 9; i++) {
            total = total + term;
            term  = term * half;
        }
        /* 511/256 ≈ 1.996; trunc is 1 */
        check("T9  geom sum trunc == 1:  ", feq_int(total, 1));

        /* total * 256 == 511 exactly. */
        float scaled = total * fpass((float)256);
        check("T9b geom*256 == 511:      ", feq_int(scaled, 511));
    }

    printf("\nResults: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
