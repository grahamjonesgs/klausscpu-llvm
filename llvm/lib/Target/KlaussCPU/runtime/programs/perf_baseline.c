// perf_baseline.c — KlaussCPU CPU/cache performance baseline.
//
// Reads the MMIO performance counters (CPU pipeline counters at 0xF00D, cache
// counters at 0xF005, millisecond clock at 0xF00F) and reports CPI, instruction
// mix, branch behaviour, cycle accounting, and cache hit/miss/penalty for a
// suite of micro-kernels that each isolate one behaviour:
//
//   alu         tight integer ALU dependency chain     -> best-case CPI, ALU mix
//   mem_stream  sequential R/W over a >L1 buffer        -> miss rate, writebacks
//   ptr_chase   dependent random pointer chase (>L1)    -> worst-case miss penalty
//   branchy     data-dependent branches (xorshift PRNG) -> branch fraction + taken%
//   calls(fib)  naive recursive fib                     -> CALL / INDIRECT(ret) mix
//   muldiv      64-bit multiply + divide loop           -> MUL/DIV ops + avg latency
//
// Method: for each kernel — clear counters, run, snapshot every counter into
// locals, THEN format/print.  Printing (and, over SSH, wolfSSH/network work) is
// outside the measured window.  Derived metrics use integer fixed-point only
// (no float printf).  64-bit values print with %lu (KlaussCPU `long` is 64-bit
// and picolibc has no `ll` modifier).
//
// Run it two ways:
//   * Bare-metal `make perf_baseline.elf`, load via the FPGA serial loader —
//     cleanest numbers (no OS activity).
//   * `make perf_baseline.llext`, copy to SD, `run perf_baseline.llext` over SSH.
//     The counters are global, so numbers include the timer ISR (~1 per 10.5 ms)
//     and any concurrent SSH/network activity — run on an otherwise-idle board.
//
// Each kernel also emits a compact "CSV," line (name, ms, cycles, instr,
// cpi_milli, missrate_bp, taken_bp) for easy diffing across runs to track
// regressions / improvements.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../mmio.h"

typedef uint64_t u64;
typedef uint32_t u32;

#define U(x) ((unsigned long)(x))   /* cast for %lu */

/* ── tunables — sized for ~0.1–0.5 s each at 100 MHz; adjust freely ──────── */
#define ALU_ITERS     500000u
#define MEM_WORDS     32768u        /* 256 KiB working set (> 64 KiB L1)      */
#define MEM_REPS      4u
#define CHASE_STEPS   200000u
#define BR_ITERS      500000u
#define FIB_N         28u
#define MD_ITERS      50000u

static volatile u64 g_sink;         /* defeats dead-code elimination          */
static u64 *g_buf;                  /* working-set buffer (heap)              */
static u32  g_words;

typedef struct {
    u64 ms, cycles, instr;
    u64 fetch, exec, mul, div, intc, idle;
    u64 mul_ops, div_ops, int_ops;
    u64 alu, load, store, branch, taken, jump, call, indirect, other;
    u64 rh, rm, wh, wm, wb, stall;
} snap_t;

/* ── fixed-point printers ────────────────────────────────────────────────── */

/* print num/den as a percentage with two decimals, e.g. "84.21%" */
static void pct(u64 num, u64 den) {
    u64 p = den ? (num * 10000ull) / den : 0;   /* hundredths of a percent */
    printf("%lu.%02lu%%", U(p / 100), U(p % 100));
}

/* print num/den as a cycle count with two decimals, e.g. "12.34c" */
static void cyc2(u64 num, u64 den) {
    u64 v = den ? (num * 100ull) / den : 0;
    printf("%lu.%02luc", U(v / 100), U(v % 100));
}

static void report(const char *name, const snap_t *s) {
    u64 misses   = s->rm + s->wm;
    u64 accesses = s->rh + s->rm + s->wh + s->wm;
    u64 mix      = s->alu + s->load + s->store + s->branch +
                   s->jump + s->call + s->indirect + s->other;
    u64 cpi_m    = s->instr ? (s->cycles * 1000ull) / s->instr : 0;

    printf("\n--- %s ---\n", name);
    printf("time=%lums cycles=%lu instr=%lu CPI=%lu.%03lu\n",
           U(s->ms), U(s->cycles), U(s->instr), U(cpi_m / 1000), U(cpi_m % 1000));

    printf("mix:  ALU=");  pct(s->alu, s->instr);
    printf(" LD=");        pct(s->load, s->instr);
    printf(" ST=");        pct(s->store, s->instr);
    printf(" BR=");        pct(s->branch, s->instr);
    printf(" JMP=");       pct(s->jump, s->instr);
    printf(" CALL=");      pct(s->call, s->instr);
    printf(" IND=");       pct(s->indirect, s->instr);
    printf(" OTH=");       pct(s->other, s->instr);
    printf("\n");
    printf("      (mix sum=%lu instr=%lu %s)\n",
           U(mix), U(s->instr), mix == s->instr ? "ok" : "MISMATCH");

    printf("branch: n=%lu taken=%lu rate=", U(s->branch), U(s->taken));
    pct(s->taken, s->branch);
    printf("\n");

    printf("cyc:  fetch="); pct(s->fetch, s->cycles);
    printf(" exec=");       pct(s->exec, s->cycles);
    printf(" mul=");        pct(s->mul, s->cycles);
    printf(" div=");        pct(s->div, s->cycles);
    printf(" int=");        pct(s->intc, s->cycles);
    printf(" idle=");       pct(s->idle, s->cycles);
    printf("\n");

    printf("cache: acc=%lu rdH=%lu rdM=%lu wrH=%lu wrM=%lu wb=%lu miss=",
           U(accesses), U(s->rh), U(s->rm), U(s->wh), U(s->wm), U(s->wb));
    pct(misses, accesses);
    printf(" stall=%lu", U(s->stall));
    if (misses) { printf(" avgpen="); cyc2(s->stall, misses); }
    printf("\n");

    if (s->mul_ops) { printf("mul: ops=%lu avglat=", U(s->mul_ops)); cyc2(s->mul, s->mul_ops); printf("\n"); }
    if (s->div_ops) { printf("div: ops=%lu avglat=", U(s->div_ops)); cyc2(s->div, s->div_ops); printf("\n"); }

    /* compact machine-readable line for cross-run diffing */
    u64 missrate_bp = accesses ? (misses * 10000ull) / accesses : 0;
    u64 taken_bp    = s->branch ? (s->taken * 10000ull) / s->branch : 0;
    printf("CSV,%s,%lu,%lu,%lu,%lu,%lu,%lu\n",
           name, U(s->ms), U(s->cycles), U(s->instr), U(cpi_m), U(missrate_bp), U(taken_bp));
}

/* ── micro-kernels ───────────────────────────────────────────────────────── */

__attribute__((noinline))
static void k_alu(void) {
    u64 a = 0x0123456789abcdefull, b = 0xfedcba9876543210ull;
    for (u32 i = 0; i < ALU_ITERS; i++) {
        a += b; a ^= (a << 7); a -= (b >> 3);
        b += a; b ^= (b << 11); b += i;
    }
    g_sink = a + b;
}

__attribute__((noinline))
static void k_mem(void) {
    volatile u64 *b = g_buf;
    u32 n = g_words;
    u64 sum = 0;
    for (u32 r = 0; r < MEM_REPS; r++) {
        for (u32 i = 0; i < n; i++) b[i] = b[i] + i + r;   /* read + write */
        for (u32 i = 0; i < n; i++) sum += b[i];            /* read        */
    }
    g_sink = sum;
}

/* Sattolo's algorithm: turn g_buf into a single-cycle permutation so the chase
   touches every slot exactly once per lap.  Not part of any measured window. */
static void build_perm(void) {
    u64 *b = g_buf;
    u32 n = g_words;
    for (u32 i = 0; i < n; i++) b[i] = i;
    u32 rng = 0x2545f491u;
    for (u32 i = n - 1; i > 0; i--) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        u32 j = rng % i;                 /* 0..i-1 */
        u64 t = b[i]; b[i] = b[j]; b[j] = t;
    }
}

__attribute__((noinline))
static void k_chase(void) {
    volatile u64 *b = g_buf;
    u32 idx = 0;
    u64 acc = 0;
    for (u32 i = 0; i < CHASE_STEPS; i++) { idx = (u32)b[idx]; acc += idx; }
    g_sink = acc;
}

__attribute__((noinline))
static void k_branch(void) {
    u32 rng = 0x1234567u;
    u64 cnt = 0;
    for (u32 i = 0; i < BR_ITERS; i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;   /* pure ALU */
        if (rng & 0x80000000u) cnt++;          /* ~50% taken */
        if ((rng & 0xffu) < 64u) cnt += 2;     /* ~25% taken */
        if ((rng & 0x0f00u) == 0) cnt += 3;    /* ~6%  taken */
    }
    g_sink = cnt;
}

__attribute__((noinline))
static u64 fib(u32 n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}
__attribute__((noinline))
static void k_calls(void) { g_sink = fib(FIB_N); }

__attribute__((noinline))
static void k_muldiv(void) {
    u64 a = 0x9e3779b97f4a7c15ull, acc = 0;
    for (u32 i = 1; i <= MD_ITERS; i++) {
        a  = a * (u64)i;             /* 64-bit multiply */
        acc += a / (u64)(i | 1u);    /* 64-bit divide   */
        acc ^= a % (u64)(i | 3u);    /* 64-bit modulo   */
    }
    g_sink = acc;
}

/* ── harness ─────────────────────────────────────────────────────────────── */

static void bench(const char *name, void (*fn)(void)) {
    REG_CACHE_CTRL = CACHE_CTRL_CLEAR;
    REG_PERF_CTRL  = PERF_CTRL_CLEAR;
    u64 t0 = REG_CLOCK_MS;
    fn();
    u64 t1 = REG_CLOCK_MS;

    snap_t s;
    s.cycles   = REG_PERF_CYCLES;   s.instr  = REG_PERF_INSTR;
    s.fetch    = REG_PERF_FETCH_CYCLES; s.exec = REG_PERF_EXEC_CYCLES;
    s.mul      = REG_PERF_MUL_CYCLES;   s.div  = REG_PERF_DIV_CYCLES;
    s.intc     = REG_PERF_INT_CYCLES;   s.idle = REG_PERF_IDLE_CYCLES;
    s.mul_ops  = REG_PERF_MUL_OPS;  s.div_ops = REG_PERF_DIV_OPS;  s.int_ops = REG_PERF_INT_OPS;
    s.alu      = REG_PERF_CNT_ALU;  s.load   = REG_PERF_CNT_LOAD;  s.store = REG_PERF_CNT_STORE;
    s.branch   = REG_PERF_CNT_BRANCH; s.taken = REG_PERF_CNT_BRANCH_TAKEN;
    s.jump     = REG_PERF_CNT_JUMP; s.call   = REG_PERF_CNT_CALL;
    s.indirect = REG_PERF_CNT_INDIRECT; s.other = REG_PERF_CNT_OTHER;
    s.rh = REG_CACHE_RD_HITS;  s.rm = REG_CACHE_RD_MISSES;
    s.wh = REG_CACHE_WR_HITS;  s.wm = REG_CACHE_WR_MISSES;
    s.wb = REG_CACHE_WRITEBACKS; s.stall = REG_CACHE_STALL_CYC;
    s.ms = t1 - t0;

    report(name, &s);
}

int main(void) {
    printf("=== KlaussCPU perf baseline ===\n");

    u64 info = REG_CACHE_INFO;
    printf("L1: ways=%u sets=%u line=%uB total=%uB\n",
           CACHE_INFO_WAYS(info), CACHE_INFO_SETS(info),
           CACHE_INFO_LINE_BYTES(info), CACHE_INFO_TOTAL_BYTES(info));

    g_words = MEM_WORDS;
    g_buf = (u64 *)malloc((size_t)g_words * sizeof(u64));
    if (!g_buf) {
        printf("malloc(%u KiB) failed\n", (unsigned)(g_words * 8u / 1024u));
        return 1;
    }
    printf("working set: %u KiB\n\n", (unsigned)(g_words * 8u / 1024u));

    bench("alu",        k_alu);
    bench("mem_stream", k_mem);
    build_perm();
    bench("ptr_chase",  k_chase);
    bench("branchy",    k_branch);
    bench("calls_fib",  k_calls);
    bench("muldiv",     k_muldiv);

    free(g_buf);
    printf("\n=== done (sink=%lu) ===\n", U(g_sink));
    return 0;
}
