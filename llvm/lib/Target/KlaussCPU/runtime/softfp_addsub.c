/* softfp_addsub.c — KlaussCPU soft-FP add/subtract using a while-loop
 * renormalisation, avoiding the hardware CLZ instruction.
 *
 * compiler-rt's __addsf3 uses rep_clz() → __builtin_clz() for the
 * renormalisation shift after a cancelling subtraction.  On KlaussCPU the
 * compiler expands __builtin_clz(x) as "shlr r, x, 32; clz r" assuming a
 * 64-bit CLZ instruction, but the hardware CLZ only examines the lower 32
 * bits of the register — the value shifted to the upper half is invisible,
 * giving clz(0) = 32 and a wrong shift count.
 *
 * This file provides __addsf3 / __subsf3 / __negsf2 using the same
 * IEEE-correct bit-manipulation as the old softfp.c, with a simple
 * while-loop for normalisation that needs no CLZ instruction.
 */

typedef unsigned int       uint32_t;
typedef int                int32_t;
typedef unsigned long long uint64_t;
typedef long long          int64_t;

union fp32 { float f; uint32_t u; };

static uint32_t f2u(float f) { union fp32 v; v.f = f; return v.u; }
static float    u2f(uint32_t u) { union fp32 v; v.u = u; return v.f; }

#define BIAS  127
#define MMASK 0x007FFFFFu
#define EMASK 0xFFu
#define EBITS 23

static uint32_t mkfp(int sign, int exp, uint32_t mant) {
    return ((uint32_t)sign << 31) | ((uint32_t)(exp & EMASK) << EBITS) | (mant & MMASK);
}

static uint32_t round_normal(uint32_t mag25, int round, int sticky, int *exp) {
    uint32_t mant = mag25 & MMASK;
    if (round && (sticky || (mant & 1))) {
        mant++;
        if (mant == (1u << EBITS)) {
            mant = 0;
            (*exp)++;
        }
    }
    return mant;
}

float __negsf2(float a) {
    return u2f(f2u(a) ^ 0x80000000u);
}

float __addsf3(float fa, float fb) {
    uint32_t a = f2u(fa);
    uint32_t b = f2u(fb);

    if ((a & 0x7FFFFFFFu) == 0) return fb;
    if ((b & 0x7FFFFFFFu) == 0) return fa;

    int sa = (int)(a >> 31), sb = (int)(b >> 31);
    int ea = (int)((a >> EBITS) & EMASK);
    int eb = (int)((b >> EBITS) & EMASK);
    uint32_t ma = (a & MMASK) | (1u << EBITS);
    uint32_t mb = (b & MMASK) | (1u << EBITS);

    int sticky = 0;
    if (ea > eb) {
        int d = ea - eb;
        if (d > 31) { sticky = (mb != 0); mb = 0; }
        else { sticky = (mb & ((1u << d) - 1)) != 0; mb >>= d; }
    } else if (eb > ea) {
        int d = eb - ea;
        if (d > 31) { sticky = (ma != 0); ma = 0; }
        else { sticky = (ma & ((1u << d) - 1)) != 0; ma >>= d; }
        ea = eb;
    }

    int sign;
    uint32_t mag;
    if (sa == sb) {
        sign = sa;
        mag  = ma + mb;
        if (mag & (1u << (EBITS + 1))) {
            sticky |= (mag & 1);
            mag >>= 1;
            ea++;
        }
    } else {
        if (ma >= mb) { sign = sa; mag = ma - mb; }
        else          { sign = sb; mag = mb - ma; }
        if (mag == 0 && sticky == 0) return u2f(0);

        while ((mag & (1u << EBITS)) == 0 && ea > 0) {
            mag <<= 1;
            ea--;
        }
    }

    int exp = ea;
    uint32_t mant = round_normal(mag, 0, sticky, &exp);
    return u2f(mkfp(sign, exp, mant));
}

float __subsf3(float a, float b) {
    return __addsf3(a, __negsf2(b));
}
