/*
 * test_rtos.c — KlaussCPU preemptive RTOS smoke test.
 *
 * Creates four tasks that exercise three different scheduling modes:
 *
 *   task_a — cooperative yield: calls rtos_yield() between iterations.
 *             Proves voluntary CPU hand-off works.
 *
 *   task_b — timed sleep: calls rtos_sleep_ticks(SLEEP_TICKS) between
 *             iterations.  Proves timed blocking and wake-up work.
 *
 *   task_c — preemptive: busy-waits so the timer fires mid-spin.
 *             Unchanged from the original test; serves as baseline.
 *
 *   reporter — waits for all workers; verifies tick count advanced by at
 *              least ITERS*SLEEP_TICKS ticks (proves task_b actually slept),
 *              then prints a summary and signals PASS/FAIL.
 *
 * Success: REG_LEDS = 0xFFFF and 7-seg shows "CAFE".
 */

#include <stdio.h>
#include "../mmio.h"
#include "../rtos.h"

#define ITERS       8   /* iterations per worker task */
#define SLEEP_TICKS 3   /* ticks task_b sleeps between iterations */

/* Shared completion flags — written by workers, read by reporter. */
static volatile int done[3];

/* ── Worker tasks ──────────────────────────────────────────────────────────
 * printf is wrapped in rtos_mutex_lock/unlock so picolibc's shared stdout
 * state is not accessed concurrently by two tasks.
 * ─────────────────────────────────────────────────────────────────────────*/

/* task_a: cooperative yield between iterations. */
static void task_a(void) {
    for (int i = 0; i < ITERS; i++) {
        REG_LEDS = 0x0001;
        rtos_mutex_lock();
        printf("[A] %d (yield)\n", i);
        rtos_mutex_unlock();
        /* Yield the CPU several times instead of busy-waiting. */
        for (int j = 0; j < 5; j++) rtos_yield();
    }
    done[0] = 1;
    while (1) { REG_LEDS = 0x0001; }
}

/* task_b: timed sleep between iterations. */
static void task_b(void) {
    for (int i = 0; i < ITERS; i++) {
        REG_LEDS = 0x0002;
        rtos_mutex_lock();
        printf("[B] %d (sleep %d ticks)\n", i, SLEEP_TICKS);
        rtos_mutex_unlock();
        rtos_sleep_ticks(SLEEP_TICKS);
    }
    done[1] = 1;
    while (1) { REG_LEDS = 0x0002; }
}

/* task_c: preemptive busy-wait — timer fires mid-spin. */
static void task_c(void) {
    for (int i = 0; i < ITERS; i++) {
        REG_LEDS = 0x0004;
        rtos_mutex_lock();
        printf("[C] %d (preempt)\n", i);
        rtos_mutex_unlock();
        for (volatile long j = 0; j < 200000L; j++) {}
    }
    done[2] = 1;
    while (1) { REG_LEDS = 0x0004; }
}

/* ── Reporter task ─────────────────────────────────────────────────────────
 * Wakes every ~500 k cycles, polls for completion, then prints results.
 * This is the only place printf is called from a "long-lived" perspective;
 * worker printf calls may be interleaved with it (output may mix), but
 * the UART driver is character-atomic so nothing crashes.
 * ─────────────────────────────────────────────────────────────────────────*/

static void reporter(void) {
    /* Wait until all three workers have finished. */
    while (!(done[0] && done[1] && done[2]))
        for (volatile long j = 0; j < 500000L; j++) {}

    uint32_t ticks = g_tick_count;
    uint32_t min_expected_ticks = (uint32_t)(ITERS * SLEEP_TICKS);

    printf("\n=== all tasks done ===\n");
    printf("scheduling modes: A=yield  B=sleep(%d ticks)  C=preempt\n", SLEEP_TICKS);
    printf("context switches:\n");
    printf("  [A yield  ] %lu\n", (unsigned long)rtos_task_switches(0));
    printf("  [B sleep  ] %lu\n", (unsigned long)rtos_task_switches(1));
    printf("  [C preempt] %lu\n", (unsigned long)rtos_task_switches(2));
    printf("  [reporter ] %lu\n", (unsigned long)rtos_task_switches(3));
    printf("tick count: %lu (need >= %lu for sleep test)\n",
           (unsigned long)ticks, (unsigned long)min_expected_ticks);

    /* All tasks must have been scheduled at least ITERS times. */
    int switches_ok = (rtos_task_switches(0) >= (uint32_t)ITERS) &&
                      (rtos_task_switches(1) >= (uint32_t)ITERS) &&
                      (rtos_task_switches(2) >= (uint32_t)ITERS);

    /* Tick counter must reflect task_b's sleep duration. */
    int sleep_ok = (ticks >= min_expected_ticks);

    int ok = switches_ok && sleep_ok;
    printf("switches: %s  sleep timing: %s\n",
           switches_ok ? "PASS" : "FAIL",
           sleep_ok    ? "PASS" : "FAIL");
    printf("Result: %s\n", ok ? "PASS" : "FAIL");

    REG_SEG_ALL = ok ? 0xCAFE0000u : 0x000000FFu;
    REG_LEDS    = ok ? 0xFFFFu     : 0x0001u;

    while (1) {}
}

/* ── main ──────────────────────────────────────────────────────────────────*/

int main(void) {
    REG_LEDS = 0x0000;
    printf("=== KlaussCPU RTOS test ===\n");
    printf("timer period: %d cycles (10 ms @ 100 MHz)\n", RTOS_TICK_CYCLES);
    printf("tasks: A(yield) B(sleep %d ticks) C(preempt) + reporter\n",
           SLEEP_TICKS);

    rtos_init();
    task_create("A",        task_a);
    task_create("B",        task_b);
    task_create("C",        task_c);
    task_create("reporter", reporter);

    printf("Starting RTOS...\n");
    rtos_start();   /* never returns */
    return 0;
}
