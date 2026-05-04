/* softfp.c — Minimal IEEE 754 single-precision soft-FP runtime for KlaussCPU.
 *
 * Provides the compiler-rt ABI symbols that LLVM emits when expanding
 * `float` operations on a target without hardware FP.  This is a *limited*
 * implementation aimed at the runtime test programs:
 *
 *   - Single precision only (`float` / `binary32`).  No `double`.
 *   - Round-to-nearest-even.
 *   - NORMAL values only — subnormals are flushed to zero, and Inf/NaN are
 *     not handled specially (operations on or producing them yield garbage).
 *   - No exception flags, no rounding-mode control.
 *
 * If you need a fully-conforming runtime, vendor compiler-rt's soft-FP
 * builtins (already part of this LLVM tree at compiler-rt/lib/builtins/);
 * the symbol names below are ABI-compatible.
 *
 * Implemented libcalls:
 *   __floatsisf, __floatunsisf, __fixsfsi, __fixunssfsi
 *   __addsf3, __subsf3, __mulsf3, __divsf3, __negsf2
 *   __eqsf2, __nesf2, __ltsf2, __lesf2, __gtsf2, __gesf2
 */

typedef unsigned int       uint32_t;
typedef int                int32_t;
typedef unsigned long long uint64_t;
typedef long long          int64_t;

/* Bit-level access to a float's representation. */
union fp32 { float f; uint32_t u; };

static uint32_t f2u(float f) { union fp32 v; v.f = f; return v.u; }
static float    u2f(uint32_t u) { union fp32 v; v.u = u; return v.f; }

#define BIAS  127
#define MMASK 0x007FFFFFu     /* 23-bit mantissa mask */
#define EMASK 0xFFu           /* 8-bit exponent mask */
#define EBITS 23

/* Build a float from sign / biased-exponent / 23-bit mantissa fields. */
static uint32_t mkfp(int sign, int exp, uint32_t mant) {
    return ((uint32_t)sign << 31) | ((uint32_t)(exp & EMASK) << EBITS) | (mant & MMASK);
}

/* Round a 25-bit aligned magnitude (bit 24 = implicit 1 of significand,
 * bits 23..1 = explicit mantissa, bit 0 = round/sticky combined) to a
 * 24-bit significand.  Round-to-nearest-even.  Adjusts `*exp` if the round
 * carries past bit 24. */
static uint32_t round_normal(uint32_t mag25, int round, int sticky, int *exp) {
    /* mag25 has bit 24 set (the implicit leading 1) and 23 explicit bits. */
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

/* ── Conversions ──────────────────────────────────────────────────────────── */

/* Signed 32-bit int → float. */
float __floatsisf(int32_t a) {
    if (a == 0) return u2f(0);
    int sign = (a < 0);
    uint32_t mag = sign ? (uint32_t)(-(int64_t)a) : (uint32_t)a;

    /* Normalize: shift leading 1 to bit 31. */
    int lz = 0;
    while ((mag & 0x80000000u) == 0) { mag <<= 1; lz++; }
    int exp_unb = 31 - lz;

    /* mag = 1aaaaaaaaaaaaaaaaaaaaaaaa rrrrrrrr  (1 + 23 explicit + 8 round/sticky) */
    uint32_t mantissa = (mag >> 8) & MMASK;
    int round  = (mag >> 7) & 1;
    int sticky = (mag & 0x7Fu) != 0;
    int exp = exp_unb + BIAS;
    mantissa = round_normal((1u << EBITS) | mantissa, round, sticky, &exp);
    return u2f(mkfp(sign, exp, mantissa));
}

/* Unsigned 32-bit int → float. */
float __floatunsisf(uint32_t a) {
    if (a == 0) return u2f(0);
    int lz = 0;
    while ((a & 0x80000000u) == 0) { a <<= 1; lz++; }
    int exp_unb = 31 - lz;

    uint32_t mantissa = (a >> 8) & MMASK;
    int round  = (a >> 7) & 1;
    int sticky = (a & 0x7Fu) != 0;
    int exp = exp_unb + BIAS;
    mantissa = round_normal((1u << EBITS) | mantissa, round, sticky, &exp);
    return u2f(mkfp(0, exp, mantissa));
}

/* Float → signed 64-bit int (truncating, like C cast).
 * Needed because `long` is 64-bit on KlaussCPU. */
int64_t __fixsfdi(float fa) {
    uint32_t a = f2u(fa);
    int sign = (int)(a >> 31);
    int exp  = (int)((a >> EBITS) & EMASK);
    uint32_t mant = (a & MMASK) | (exp ? (1u << EBITS) : 0);

    int unbiased = exp - BIAS;
    if (exp == 0)        return 0;
    if (unbiased < 0)    return 0;
    if (unbiased > 62)   return sign ? (int64_t)0x8000000000000000ULL : 0x7FFFFFFFFFFFFFFFLL;

    uint64_t result;
    if (unbiased >= EBITS) result = (uint64_t)mant << (unbiased - EBITS);
    else                   result = (uint64_t)mant >> (EBITS - unbiased);

    return sign ? -(int64_t)result : (int64_t)result;
}

/* Float → signed 32-bit int (truncating, like C cast).
 * Out-of-range and NaN return INT32_MAX / INT32_MIN per compiler-rt. */
int32_t __fixsfsi(float fa) {
    uint32_t a = f2u(fa);
    int sign = (int)(a >> 31);
    int exp  = (int)((a >> EBITS) & EMASK);
    uint32_t mant = (a & MMASK) | (exp ? (1u << EBITS) : 0);   /* implicit 1 */

    int unbiased = exp - BIAS;
    if (exp == 0)        return 0;        /* zero / subnormal flushed */
    if (unbiased < 0)    return 0;        /* |a| < 1 truncates to 0 */
    if (unbiased > 30)   return sign ? (int32_t)0x80000000u : 0x7FFFFFFF;

    /* mant has bit 23 = implicit leading 1.  Shift to align bit 23 with
     * the integer's bit `unbiased`. */
    uint32_t result;
    if (unbiased >= EBITS) result = mant << (unbiased - EBITS);
    else                   result = mant >> (EBITS - unbiased);

    return sign ? -(int32_t)result : (int32_t)result;
}

/* Float → unsigned 32-bit int (truncating). */
uint32_t __fixunssfsi(float fa) {
    uint32_t a = f2u(fa);
    int sign = (int)(a >> 31);
    int exp  = (int)((a >> EBITS) & EMASK);
    if (sign || exp == 0) return 0;
    int unbiased = exp - BIAS;
    if (unbiased < 0)    return 0;
    if (unbiased > 31)   return 0xFFFFFFFFu;
    uint32_t mant = (a & MMASK) | (1u << EBITS);
    if (unbiased >= EBITS) return mant << (unbiased - EBITS);
    return mant >> (EBITS - unbiased);
}

/* ── Negation ─────────────────────────────────────────────────────────────── */

float __negsf2(float a) {
    return u2f(f2u(a) ^ 0x80000000u);
}

/* ── Add / Sub ────────────────────────────────────────────────────────────── */

float __addsf3(float fa, float fb) {
    uint32_t a = f2u(fa);
    uint32_t b = f2u(fb);

    /* Trivial zero passthrough. */
    if ((a & 0x7FFFFFFFu) == 0) return fb;
    if ((b & 0x7FFFFFFFu) == 0) return fa;

    int sa = (int)(a >> 31), sb = (int)(b >> 31);
    int ea = (int)((a >> EBITS) & EMASK);
    int eb = (int)((b >> EBITS) & EMASK);
    /* Significands with implicit 1: 24-bit values. */
    uint32_t ma = (a & MMASK) | (1u << EBITS);
    uint32_t mb = (b & MMASK) | (1u << EBITS);

    /* Align: shift the smaller significand right by the exponent diff,
     * tracking sticky bits.  Use 32-bit arithmetic with 8 guard bits. */
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
        /* Same sign — just add. */
        sign = sa;
        mag  = ma + mb;
        if (mag & (1u << (EBITS + 1))) {
            /* Carried into bit 24: shift right one, drop one of the sticky bits. */
            sticky |= (mag & 1);
            mag >>= 1;
            ea++;
        }
    } else {
        /* Different sign — subtract smaller magnitude from larger. */
        if (ma >= mb) { sign = sa; mag = ma - mb; }
        else          { sign = sb; mag = mb - ma; }
        if (mag == 0 && sticky == 0) return u2f(0);     /* exact cancellation */

        /* Renormalize: leading 1 should be at bit 23.  Sticky carries the
         * "any-bit-below" information from the alignment phase and is
         * preserved across the shifts (it represents bits below the new LSB
         * just as it did the old LSB). */
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

/* ── Multiply ─────────────────────────────────────────────────────────────── */

float __mulsf3(float fa, float fb) {
    uint32_t a = f2u(fa);
    uint32_t b = f2u(fb);

    if ((a & 0x7FFFFFFFu) == 0 || (b & 0x7FFFFFFFu) == 0)
        return u2f(((a ^ b) & 0x80000000u));     /* signed zero */

    int sa = (int)(a >> 31), sb = (int)(b >> 31);
    int ea = (int)((a >> EBITS) & EMASK);
    int eb = (int)((b >> EBITS) & EMASK);
    uint32_t ma = (a & MMASK) | (1u << EBITS);
    uint32_t mb = (b & MMASK) | (1u << EBITS);

    /* 24x24 → 48-bit product. */
    uint64_t prod = (uint64_t)ma * (uint64_t)mb;

    /* Leading 1 of prod is at bit 46 or bit 47.  Significands are each in
     * [1,2), so their product is in [1,4).  Base exponent accounts for the
     * case where leading 1 is at bit 47 (product ≥ 2); if it's at bit 46
     * instead, we shift left and decrement. */
    int exp = ea + eb - BIAS + 1;
    if ((prod & (1ULL << 47)) == 0) {
        prod <<= 1;
        exp--;
    }

    /* Extract: 24 bits = prod[47..24]; the low 24 bits are round+sticky. */
    uint32_t mag    = (uint32_t)(prod >> 24);
    int round       = (int)((prod >> 23) & 1);
    int sticky      = (prod & ((1ULL << 23) - 1)) != 0;

    int sign = sa ^ sb;
    uint32_t mant = round_normal(mag, round, sticky, &exp);
    return u2f(mkfp(sign, exp, mant));
}

/* ── Divide ───────────────────────────────────────────────────────────────── */

float __divsf3(float fa, float fb) {
    uint32_t a = f2u(fa);
    uint32_t b = f2u(fb);

    int sa = (int)(a >> 31), sb = (int)(b >> 31);
    int sign = sa ^ sb;

    if ((b & 0x7FFFFFFFu) == 0)
        return u2f(((uint32_t)sign << 31) | 0x7F800000u);   /* +/- Inf */
    if ((a & 0x7FFFFFFFu) == 0)
        return u2f((uint32_t)sign << 31);                   /* signed zero */

    int ea = (int)((a >> EBITS) & EMASK);
    int eb = (int)((b >> EBITS) & EMASK);
    uint32_t ma = (a & MMASK) | (1u << EBITS);
    uint32_t mb = (b & MMASK) | (1u << EBITS);

    /* Long division: ma * 2^25 / mb, giving a 25-bit quotient with one round
     * + one sticky-bit position to spare for rounding. */
    uint64_t numer = (uint64_t)ma << 25;
    uint64_t q = numer / mb;
    uint64_t rem = numer - q * mb;

    /* Quotient (ma/mb) ∈ [0.5,2).  If ratio < 1 (bit 25 not set), the
     * significand is in [0.5,1) and needs a ×2 normalisation which costs
     * one exponent unit; account for that here so both branches land right. */
    int exp = ea - eb + BIAS - 1;

    /* q is 25 bits.  Leading 1 should be at bit 24; if it's at bit 25,
     * shift right and bump exp. */
    if (q & (1ULL << 25)) {
        /* Combine the bit being dropped into rem-sticky equivalent. */
        if (q & 1) rem = rem | 1;       /* OR in low bit as sticky */
        q >>= 1;
        exp++;
    }

    int round  = (int)(q & 1);
    uint32_t mag = (uint32_t)(q >> 1);
    int sticky = (rem != 0);

    uint32_t mant = round_normal(mag, round, sticky, &exp);
    return u2f(mkfp(sign, exp, mant));
}

/* ── Comparisons ──────────────────────────────────────────────────────────── */

/* Compiler-rt comparison ABI:
 *   __eqsf2 / __nesf2: returns 0 if equal, non-zero otherwise.
 *   __ltsf2 / __lesf2: returns < 0 if a < b, 0 if a == b, > 0 if a > b.
 *   __gtsf2 / __gesf2: returns > 0 if a > b, 0 if a == b, < 0 if a < b.
 *   NaN handling skipped (we don't generate NaNs).
 */
static int compare(float fa, float fb) {
    uint32_t a = f2u(fa);
    uint32_t b = f2u(fb);
    /* +0 == -0 */
    if ((a & 0x7FFFFFFFu) == 0 && (b & 0x7FFFFFFFu) == 0) return 0;
    int sa = (int)(a >> 31), sb = (int)(b >> 31);
    if (sa != sb) return sa ? -1 : 1;
    /* Same sign: compare magnitudes; flip for negatives. */
    int cmp;
    uint32_t am = a & 0x7FFFFFFFu;
    uint32_t bm = b & 0x7FFFFFFFu;
    if      (am < bm) cmp = -1;
    else if (am > bm) cmp = 1;
    else              cmp = 0;
    return sa ? -cmp : cmp;
}

int __eqsf2(float a, float b) { return compare(a, b); }
int __nesf2(float a, float b) { return compare(a, b); }
int __ltsf2(float a, float b) { return compare(a, b); }
int __lesf2(float a, float b) { return compare(a, b); }
int __gtsf2(float a, float b) { return compare(a, b); }
int __gesf2(float a, float b) { return compare(a, b); }

/* Returns non-zero if either argument is NaN (exponent=0xFF, mantissa≠0).
 * O0 prevents -O1 from recognising the bit-pattern as fcmp uno and replacing
 * the body with a call to __unordsf2 itself (infinite recursion). */
__attribute__((optnone))
int __unordsf2(float fa, float fb) {
    uint32_t a = f2u(fa);
    uint32_t b = f2u(fb);
    int a_nan = ((a & 0x7F800000u) == 0x7F800000u) && (a & MMASK);
    int b_nan = ((b & 0x7F800000u) == 0x7F800000u) && (b & MMASK);
    return a_nan || b_nan;
}
