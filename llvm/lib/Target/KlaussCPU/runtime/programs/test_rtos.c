/*
 * test_rtos.c — KlaussCPU preemptive RTOS smoke test.
 *
 * Creates four tasks:
 *
 *   task_a / task_b / task_c — "worker" tasks.  Each iterates ITERS times,
 *     updating its per-task counter and the LED bar, then busy-waits so the
 *     10 ms timer fires and preempts it into the next task.  When done it
 *     sets done[n] = 1 and spins.
 *
 *   reporter — wakes periodically; once all workers are done it prints the
 *     context-switch count for every task and signals success.
 *
 * Interleaved output on the UART (e.g. "[A] 0", "[B] 0", "[A] 1", "[C] 0", …)
 * proves that preemption is actually firing.
 *
 * Success: REG_LEDS = 0xFFFF and 7-seg shows "CAFE".
 * Failure (stack overflow or no task runs): 7-seg shows "EEEEEEEE".
 */

#include <stdio.h>
#include "../mmio.h"
#include "../rtos.h"

#define ITERS 8   /* iterations per worker task */

/* Shared completion flags — written by workers, read by reporter. */
static volatile int done[3];

/* ── Worker tasks ──────────────────────────────────────────────────────────
 * Each printf is wrapped in rtos_mutex_lock/unlock (disables the timer
 * interrupt for the duration of the call) so picolibc's shared stdout state
 * is never accessed by two tasks concurrently.
 * ─────────────────────────────────────────────────────────────────────────*/

static void task_a(void) {
    for (int i = 0; i < ITERS; i++) {
        REG_LEDS = 0x0001;
        rtos_mutex_lock();
        printf("[A] %d\n", i);
        rtos_mutex_unlock();
        for (volatile long j = 0; j < 200000L; j++) {}
    }
    done[0] = 1;
    while (1) { REG_LEDS = 0x0001; }
}

static void task_b(void) {
    for (int i = 0; i < ITERS; i++) {
        REG_LEDS = 0x0002;
        rtos_mutex_lock();
        printf("[B] %d\n", i);
        rtos_mutex_unlock();
        for (volatile long j = 0; j < 200000L; j++) {}
    }
    done[1] = 1;
    while (1) { REG_LEDS = 0x0002; }
}

static void task_c(void) {
    for (int i = 0; i < ITERS; i++) {
        REG_LEDS = 0x0004;
        rtos_mutex_lock();
        printf("[C] %d\n", i);
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

    printf("\n=== all tasks done ===\n");
    printf("context switches:\n");
    printf("  [A] %lu\n", (unsigned long)rtos_task_switches(0));
    printf("  [B] %lu\n", (unsigned long)rtos_task_switches(1));
    printf("  [C] %lu\n", (unsigned long)rtos_task_switches(2));
    printf("  [reporter] %lu\n", (unsigned long)rtos_task_switches(3));

    /* Verify that all tasks ran at least ITERS times. */
    int ok = (rtos_task_switches(0) >= (uint32_t)ITERS) &&
             (rtos_task_switches(1) >= (uint32_t)ITERS) &&
             (rtos_task_switches(2) >= (uint32_t)ITERS);

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
    printf("tasks: A B C + reporter, %d iters each worker\n", ITERS);

    rtos_init();
    task_create("A",        task_a);
    task_create("B",        task_b);
    task_create("C",        task_c);
    task_create("reporter", reporter);

    printf("Starting RTOS...\n");
    rtos_start();   /* never returns */
    return 0;
}
