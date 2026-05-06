/*
 * test_cache.c — KlaussCPU cache performance counter test.
 *
 * Reads the cache geometry from CACHE_INFO, then runs four micro-workloads
 * and prints all six counters after each one.  Useful for:
 *
 *   - Validating the cache instrumentation after hardware changes.
 *   - Understanding hit/miss behaviour for programs near the 64 KB boundary.
 *   - Diagnosing the test_big crash (cache 64 KB, binary > 64 KB).
 *
 * Workloads
 * ---------
 *   W1 hot     256 B buffer, 1 000 sequential read passes  → ~100 % hits
 *   W2 cold    32 KB buffer, 1 pass (cold, never read)     → ~0 % hits
 *   W3 warm    same 32 KB buffer, 2nd pass                 → ~100 % hits
 *   W4 write   32 KB sequential write then read-back       → write misses
 *                                                             then read hits
 *
 * Cache geometry (current hardware):
 *   2-way set-associative, 64 KB total, 16-byte lines, 2048 sets.
 * A 32 KB buffer fills exactly one way in every set, so the 2nd pass
 * is fully resident and should produce 0 misses.
 *
 * Binary size note: g_hot (256 B) and g_cold (32 KB) land in BSS (zero-
 * init), so they cost no space in the kbt file but push _end to ~48 KB —
 * well under the 64 KB kbt-loader limit being investigated.
 */

#include <stdio.h>
#include "../mmio.h"

/* ── Buffers (BSS — zero-init, not in kbt payload) ───────────────────────── */

#define HOT_WORDS   32          /* 32 × 8 B = 256 B */
#define COLD_WORDS  4096        /* 4096 × 8 B = 32 KB */

static volatile uint64_t g_hot[HOT_WORDS];
static volatile uint64_t g_cold[COLD_WORDS];

/* ── Counter snapshot ────────────────────────────────────────────────────── */

typedef struct {
    uint64_t rd_hits, rd_miss;
    uint64_t wr_hits, wr_miss;
    uint64_t wbacks,  stalls;
} Stats;

static void cache_clear(void) {
    REG_CACHE_CTRL = CACHE_CTRL_CLEAR;
}

static Stats cache_snap(void) {
    Stats s;
    s.rd_hits = REG_CACHE_RD_HITS;
    s.rd_miss  = REG_CACHE_RD_MISSES;
    s.wr_hits  = REG_CACHE_WR_HITS;
    s.wr_miss  = REG_CACHE_WR_MISSES;
    s.wbacks   = REG_CACHE_WRITEBACKS;
    s.stalls   = REG_CACHE_STALL_CYC;
    return s;
}

static void print_stats(const char *label, const Stats *s) {
    uint64_t hits   = s->rd_hits + s->wr_hits;
    uint64_t misses = s->rd_miss + s->wr_miss;
    uint64_t total  = hits + misses;
    /* hit rate in per-mille → printed as x.y % — no FP needed */
    uint64_t pmill  = total ? hits * 1000ull / total : 0ull;
    uint64_t avgpen = misses ? s->stalls / misses : 0ull;

    printf("--- %s ---\n", label);
    printf("  rd  hits=%-10llu miss=%-10llu\n", s->rd_hits, s->rd_miss);
    printf("  wr  hits=%-10llu miss=%-10llu\n", s->wr_hits, s->wr_miss);
    printf("  writebacks=%-10llu stall_cyc=%-10llu\n", s->wbacks, s->stalls);
    printf("  total=%-10llu hit_rate=%llu.%llu%%  avg_miss=%llu cyc\n\n",
           total, pmill / 10ull, pmill % 10ull, avgpen);
}

/* ── Workloads ───────────────────────────────────────────────────────────── */

static void workload_hot(void) {
    /* Warm the write side (not counted). */
    for (int i = 0; i < HOT_WORDS; i++) g_hot[i] = (uint64_t)i + 1ull;

    cache_clear();

    volatile uint64_t sink = 0;
    for (int p = 0; p < 1000; p++)
        for (int i = 0; i < HOT_WORDS; i++)
            sink += g_hot[i];
    (void)sink;

    Stats s = cache_snap();
    print_stats("W1 hot  (256 B x 1000 passes)", &s);
}

static void workload_cold(void) {
    /* g_cold starts zeroed (BSS); first read is entirely cold. */
    cache_clear();

    volatile uint64_t sink = 0;
    for (int i = 0; i < COLD_WORDS; i++)
        sink += g_cold[i];
    (void)sink;

    Stats s = cache_snap();
    print_stats("W2 cold (32 KB, 1st pass)", &s);
}

static void workload_warm(void) {
    /* Second pass over the same data — should be fully resident. */
    cache_clear();

    volatile uint64_t sink = 0;
    for (int i = 0; i < COLD_WORDS; i++)
        sink += g_cold[i];
    (void)sink;

    Stats s = cache_snap();
    print_stats("W3 warm (32 KB, 2nd pass)", &s);
}

static void workload_write(void) {
    /* Write 32 KB sequentially (cold write misses), then read back
     * (should be warm hits since the writes filled the cache lines). */
    cache_clear();

    for (int i = 0; i < COLD_WORDS; i++) g_cold[i] = (uint64_t)i * 7ull;

    volatile uint64_t sink = 0;
    for (int i = 0; i < COLD_WORDS; i++) sink += g_cold[i];
    (void)sink;

    Stats s = cache_snap();
    print_stats("W4 write+readback (32 KB)", &s);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    REG_LEDS = 0x0001;
    printf("=== KlaussCPU cache test ===\n\n");

    /* ── Geometry ── */
    uint64_t info  = REG_CACHE_INFO;
    uint32_t ways  = CACHE_INFO_WAYS(info);
    uint32_t sets  = CACHE_INFO_SETS(info);
    uint32_t line  = CACHE_INFO_LINE_BYTES(info);
    uint32_t total = CACHE_INFO_TOTAL_BYTES(info);

    printf("Geometry: %u ways, %u sets, %u B/line, %u KB total\n",
           ways, sets, line, total / 1024u);

    int geo_ok = (ways == 2 && sets == 2048 && line == 16 && total == 65536);
    printf("Geometry check: %s\n\n", geo_ok ? "PASS" : "FAIL");

    REG_LEDS = 0x0002;
    workload_hot();

    REG_LEDS = 0x0003;
    workload_cold();

    REG_LEDS = 0x0004;
    workload_warm();

    REG_LEDS = 0x0005;
    workload_write();

    printf("=== done ===\n");

    if (geo_ok) {
        REG_SEG_ALL = 0xCA4E0000u;   /* "CA4E" upper = CACHE ok */
        REG_LEDS    = 0xFFFF;
    } else {
        REG_SEG_ALL = 0x000000FFu;
        REG_LEDS    = 0x0001;
    }
    return !geo_ok;
}
