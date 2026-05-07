/*
 * test_cache.c — KlaussCPU cache performance profiler.
 *
 * Reads cache geometry, then runs seven micro-workloads that exercise
 * different access patterns.  For each workload it reports:
 *
 *   • Wall-clock time          — from REG_CLOCK_MS (1 ms resolution)
 *   • Cycle count estimate     — from REG_TIMER_CNT (sub-ms, within one tick)
 *   • Hit rate (per-mille)     — (rd_hits + wr_hits) / total × 1000
 *   • Miss penalty             — stall_cycles / misses
 *   • Writeback rate           — writebacks / rd_misses
 *   • All six raw counters
 *
 * Workloads
 * ---------
 *   W1  hot         256 B buffer, 1000 passes         → ~100 % hits
 *   W2  cold        32 KB buffer, 1st pass            → ~0 % hits
 *   W3  warm        32 KB buffer, 2nd pass            → ~100 % hits
 *   W4  write       32 KB write then read-back        → write misses then hits
 *   W5  stride      32 KB, read every 4th line        → spatial locality test
 *   W6  thrash      128 KB buffer (2× cache)          → heavy writeback churn
 *   W7  partial     33 KB buffer (just over one way)  → associativity boundary
 *
 * Registers used from mmio.h:
 *   REG_CACHE_CTRL / INFO / RD_HITS / RD_MISSES / WR_HITS / WR_MISSES
 *   REG_CACHE_WRITEBACKS / STALL_CYC
 *   REG_CLOCK_MS   — free-running ms counter (atomic 64-bit, no setup needed)
 *   REG_TIMER_CNT  — live cycle counter within current timer period
 */

#include <stdio.h>
#include "../mmio.h"

/* ── Buffer sizes ──────────────────────────────────────────────────────────── */
#define HOT_WORDS      32        /*  32 × 8 B =   256 B */
#define WARM_WORDS   4096        /* 4096 × 8 B =  32 KB */
#define THRASH_WORDS 16384       /* 16384 × 8 B = 128 KB */
#define PARTIAL_WORDS 4225       /* 4225 × 8 B ≈  33 KB */

/* Stride in uint64_t elements = 4 × 8 B = 32 B = 2 cache lines apart */
#define STRIDE 4

static volatile uint64_t g_hot[HOT_WORDS];
static volatile uint64_t g_warm[WARM_WORDS];
static volatile uint64_t g_thrash[THRASH_WORDS];
static volatile uint64_t g_partial[PARTIAL_WORDS];

/* ── Counter snapshot ──────────────────────────────────────────────────────── */

typedef struct {
    uint64_t rd_hits, rd_miss;
    uint64_t wr_hits, wr_miss;
    uint64_t wbacks,  stalls;
    uint64_t ms_elapsed;
} Stats;

static void cache_clear(void) {
    REG_CACHE_CTRL = CACHE_CTRL_CLEAR;
}

static uint64_t ms_now(void) {
    return REG_CLOCK_MS;
}

static Stats cache_snap(uint64_t ms_start) {
    Stats s;
    s.rd_hits    = REG_CACHE_RD_HITS;
    s.rd_miss    = REG_CACHE_RD_MISSES;
    s.wr_hits    = REG_CACHE_WR_HITS;
    s.wr_miss    = REG_CACHE_WR_MISSES;
    s.wbacks     = REG_CACHE_WRITEBACKS;
    s.stalls     = REG_CACHE_STALL_CYC;
    s.ms_elapsed = ms_now() - ms_start;
    return s;
}

static void print_stats(const char *label, const Stats *s) {
    uint64_t hits    = s->rd_hits + s->wr_hits;
    uint64_t misses  = s->rd_miss + s->wr_miss;
    uint64_t total   = hits + misses;
    /* hit rate in per-mille: no FP required */
    uint64_t pmill   = total   ? hits * 1000ull / total            : 0ull;
    uint64_t avgpen  = misses  ? s->stalls / misses                : 0ull;
    uint64_t wbrate  = s->rd_miss ? s->wbacks * 100ull / s->rd_miss : 0ull;

    printf("=== %s ===\n", label);
    printf("  time   : %lu ms\n",              (unsigned long)s->ms_elapsed);
    printf("  rd     : hits=%-10lu miss=%lu\n",
           (unsigned long)s->rd_hits, (unsigned long)s->rd_miss);
    printf("  wr     : hits=%-10lu miss=%lu\n",
           (unsigned long)s->wr_hits, (unsigned long)s->wr_miss);
    printf("  wbacks : %-10lu  wb/rd_miss=%lu%%\n",
           (unsigned long)s->wbacks,  (unsigned long)wbrate);
    printf("  stalls : %-10lu  avg_miss_pen=%lu cyc\n",
           (unsigned long)s->stalls,  (unsigned long)avgpen);
    printf("  total  : %-10lu  hit_rate=%lu.%lu%%\n\n",
           (unsigned long)total,
           (unsigned long)(pmill / 10ull),
           (unsigned long)(pmill % 10ull));
}

/* ── Workloads ─────────────────────────────────────────────────────────────── */

static void w1_hot(void) {
    /* Pre-warm writes (not profiled). */
    for (int i = 0; i < HOT_WORDS; i++) g_hot[i] = (uint64_t)i + 1ull;
    cache_clear();
    uint64_t t0 = ms_now();

    volatile uint64_t sink = 0;
    for (int p = 0; p < 1000; p++)
        for (int i = 0; i < HOT_WORDS; i++)
            sink += g_hot[i];
    (void)sink;

    Stats _s = cache_snap(t0); print_stats("W1 hot  — 256 B × 1000 passes (expect ~100% hits)", &_s);
}

static void w2_cold(void) {
    /* g_warm is BSS-zeroed; first touch is entirely cold. */
    cache_clear();
    uint64_t t0 = ms_now();

    volatile uint64_t sink = 0;
    for (int i = 0; i < WARM_WORDS; i++) sink += g_warm[i];
    (void)sink;

    Stats _s = cache_snap(t0); print_stats("W2 cold — 32 KB 1st pass (expect ~0% hits)", &_s);
}

static void w3_warm(void) {
    /* Second pass — data loaded by W2 is still resident. */
    cache_clear();
    uint64_t t0 = ms_now();

    volatile uint64_t sink = 0;
    for (int i = 0; i < WARM_WORDS; i++) sink += g_warm[i];
    (void)sink;

    Stats _s = cache_snap(t0); print_stats("W3 warm — 32 KB 2nd pass (expect ~100% hits)", &_s);
}

static void w4_write(void) {
    /* Write 32 KB cold → read back (writes install lines; reads then hit). */
    cache_clear();
    uint64_t t0 = ms_now();

    for (int i = 0; i < WARM_WORDS; i++) g_warm[i] = (uint64_t)i * 7ull;

    volatile uint64_t sink = 0;
    for (int i = 0; i < WARM_WORDS; i++) sink += g_warm[i];
    (void)sink;

    Stats _s = cache_snap(t0); print_stats("W4 write+readback — 32 KB (write miss, read hit)", &_s);
}

static void w5_stride(void) {
    /* Access every STRIDE-th element: each access touches a different cache line
     * but skips several — tests spatial locality and line-fill efficiency. */
    cache_clear();
    uint64_t t0 = ms_now();

    volatile uint64_t sink = 0;
    for (int i = 0; i < WARM_WORDS; i += STRIDE) sink += g_warm[i];
    (void)sink;

    printf("  (stride=%d elements = %d B apart, cache line = 16 B)\n", STRIDE, STRIDE * 8);
    Stats _s = cache_snap(t0); print_stats("W5 stride — 32 KB every 4th element (spatial locality)", &_s);
}

static void w6_thrash(void) {
    /* 128 KB buffer = 2× cache capacity.  Sequential scan evicts earlier lines
     * before we can revisit them — maximum writeback churn. */
    /* First: pre-dirty the buffer with writes so writebacks actually fire. */
    for (int i = 0; i < THRASH_WORDS; i++) g_thrash[i] = (uint64_t)i;
    cache_clear();
    uint64_t t0 = ms_now();

    volatile uint64_t sink = 0;
    for (int i = 0; i < THRASH_WORDS; i++) sink += g_thrash[i];
    (void)sink;

    Stats _s = cache_snap(t0); print_stats("W6 thrash — 128 KB (2× cache, dirty evictions expected)", &_s);
}

static void w7_partial(void) {
    /* 33 KB = just over one way per set.  The second way starts spilling into
     * the first, revealing the 2-way associativity boundary. */
    cache_clear();
    uint64_t t0 = ms_now();

    volatile uint64_t sink = 0;
    for (int i = 0; i < PARTIAL_WORDS; i++) sink += g_partial[i];
    (void)sink;

    printf("  (33 KB > 32 KB per way — probes associativity limit)\n");
    Stats _s = cache_snap(t0); print_stats("W7 partial — 33 KB (just over one cache way)", &_s);
}

/* ── main ──────────────────────────────────────────────────────────────────── */

int main(void) {
    REG_LEDS = 0x0001;
    printf("=== KlaussCPU cache profiler ===\n\n");

    /* ── Cache geometry ── */
    uint64_t info  = REG_CACHE_INFO;
    uint32_t ways  = CACHE_INFO_WAYS(info);
    uint32_t sets  = CACHE_INFO_SETS(info);
    uint32_t line  = CACHE_INFO_LINE_BYTES(info);
    uint32_t total = CACHE_INFO_TOTAL_BYTES(info);

    printf("Cache geometry:\n");
    printf("  ways        = %u\n",        (unsigned)ways);
    printf("  sets        = %u\n",        (unsigned)sets);
    printf("  line size   = %u bytes\n",  (unsigned)line);
    printf("  total size  = %u KB\n\n",   (unsigned)(total / 1024u));

    /* REG_CLOCK_MS: sanity-check it's ticking (read twice, 1 ms apart). */
    uint64_t ms_a = REG_CLOCK_MS;
    for (volatile int d = 0; d < 100000; d++) {}  /* ~1 ms busy delay */
    uint64_t ms_b = REG_CLOCK_MS;
    printf("CLOCK_MS tick check: %lu → %lu (delta=%lu ms)\n\n",
           (unsigned long)ms_a,
           (unsigned long)ms_b,
           (unsigned long)(ms_b - ms_a));

    int geo_ok = (ways == 2 && sets == 2048 && line == 16 && total == 65536);
    printf("Geometry check: %s\n\n", geo_ok ? "PASS" : "FAIL");

    /* ── Run workloads ── */
    REG_LEDS = 0x0002; w1_hot();
    REG_LEDS = 0x0004; w2_cold();
    REG_LEDS = 0x0006; w3_warm();
    REG_LEDS = 0x0008; w4_write();
    REG_LEDS = 0x000A; w5_stride();
    REG_LEDS = 0x000C; w6_thrash();
    REG_LEDS = 0x000E; w7_partial();

    printf("=== profiler done ===\n");

    if (geo_ok) {
        REG_SEG_ALL = 0xCA4E0000u;  /* "CA4E" = CACHE ok */
        REG_LEDS    = 0xFFFF;
    } else {
        REG_SEG_ALL = 0x000000FFu;
        REG_LEDS    = 0x0001;
    }
    return !geo_ok;
}
