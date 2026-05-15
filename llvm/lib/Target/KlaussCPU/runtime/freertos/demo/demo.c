/*
 * demo.c — FreeRTOS KlaussCPU demo / smoke test.
 *
 * Four tasks exercise three scheduling modes:
 *
 *   TaskA (priority 2) — cooperative: calls vTaskDelay() between iterations.
 *   TaskB (priority 2) — timed sleep: vTaskDelay with fixed period.
 *   TaskC (priority 2) — preemptive: busy-loop; timer fires mid-spin.
 *   Reporter (priority 3) — waits for all workers then prints stats.
 *
 * Success: LEDs = 0xFFFF, 7-seg shows 0xCAFE.
 * Failure: LEDs = 0x0001, 7-seg shows 0x00FF.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "mmio.h"    /* REG_LEDS, REG_SEG_ALL */

/* ── Config ─────────────────────────────────────────────────────────────── */

#define ITERS           8
#define SLEEP_TICKS     5      /* vTaskDelay ticks for TaskB */
#define TASK_STACK      512    /* words × 8 bytes = 4 KB */
#define REPORTER_STACK  512

/* ── Shared state ──────────────────────────────────────────────────────── */

static volatile int done[3];   /* set by each worker on completion */
static SemaphoreHandle_t xPrintMutex;   /* serialise printf */

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void safe_printf(const char *fmt, ...) {
    va_list ap;
    xSemaphoreTake(xPrintMutex, portMAX_DELAY);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    xSemaphoreGive(xPrintMutex);
}

/* ── Worker tasks ─────────────────────────────────────────────────────────── */

static void TaskA(void *pvParam) {
    (void)pvParam;
    for (int i = 0; i < ITERS; i++) {
        REG_LEDS = 0x0001;
        safe_printf("[A] %d — yield\n", i);
        vTaskDelay(1);    /* yield for at least one tick */
    }
    done[0] = 1;
    vTaskSuspend(NULL);
}

static void TaskB(void *pvParam) {
    (void)pvParam;
    for (int i = 0; i < ITERS; i++) {
        REG_LEDS = 0x0002;
        safe_printf("[B] %d — delay %d ticks\n", i, SLEEP_TICKS);
        vTaskDelay(SLEEP_TICKS);
    }
    done[1] = 1;
    vTaskSuspend(NULL);
}

static void TaskC(void *pvParam) {
    (void)pvParam;
    for (int i = 0; i < ITERS; i++) {
        REG_LEDS = 0x0004;
        safe_printf("[C] %d — preempt spin\n", i);
        /* Busy-spin so the timer fires mid-loop, testing preemption. */
        for (volatile long j = 0; j < 200000L; j++) {}
    }
    done[2] = 1;
    vTaskSuspend(NULL);
}

/* ── Reporter ─────────────────────────────────────────────────────────────── */

static void Reporter(void *pvParam) {
    (void)pvParam;

    /* Poll until all three workers are done. */
    while (!(done[0] && done[1] && done[2]))
        vTaskDelay(10);

    TickType_t ticks = xTaskGetTickCount();
    TickType_t min_expected = (TickType_t)(ITERS * SLEEP_TICKS);

    printf("\n=== all tasks done ===\n");
    printf("  tick count : %lu  (need >= %lu for TaskB sleep test)\n",
           (unsigned long)ticks, (unsigned long)min_expected);

    int ok = (ticks >= min_expected);
    printf("Result: %s\n", ok ? "PASS" : "FAIL");

    REG_SEG_ALL = ok ? 0xCAFE0000u : 0x000000FFu;
    REG_LEDS    = ok ? 0xFFFFu     : 0x0001u;

    vTaskSuspend(NULL);
}

/* ── configASSERT handler ────────────────────────────────────────────────── */

void vAssertCalled(const char *file, unsigned long line) {
    printf("ASSERT FAILED: %s:%lu\n", file, line);
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    REG_LEDS = 0x0000;
    printf("=== KlaussCPU FreeRTOS demo ===\n");
    printf("tick rate  : %lu Hz  (%lu ms per tick)\n",
           (unsigned long)configTICK_RATE_HZ,
           (unsigned long)portTICK_PERIOD_MS);
    printf("timer freq : %lu Hz\n", (unsigned long)configCPU_CLOCK_HZ);
    printf("heap       : %u bytes\n", (unsigned)configTOTAL_HEAP_SIZE);

    xPrintMutex = xSemaphoreCreateMutex();
    configASSERT(xPrintMutex != NULL);

    xTaskCreate(TaskA,    "A",        TASK_STACK,     NULL, 2, NULL);
    xTaskCreate(TaskB,    "B",        TASK_STACK,     NULL, 2, NULL);
    xTaskCreate(TaskC,    "C",        TASK_STACK,     NULL, 2, NULL);
    xTaskCreate(Reporter, "Reporter", REPORTER_STACK, NULL, 3, NULL);

    printf("starting scheduler...\n");
    vTaskStartScheduler();

    /* Should never reach here. */
    printf("ERROR: scheduler returned\n");
    return 1;
}
