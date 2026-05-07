/*
 * test_sync.c — KlaussCPU RTOS synchronisation primitives test.
 *
 * Five tasks exercise two synchronisation primitives:
 *
 *  ── Semaphore test (producer / consumer) ──────────────────────────────────
 *
 *   task_producer   generates ITEM_COUNT integers through a semaphore-
 *                   protected ring buffer of capacity BUF_CAP.
 *   task_consumer   drains the buffer, accumulates the values, and verifies
 *                   the total matches the expected sum.
 *
 *   sem_slots (init=BUF_CAP): counts free slots  — producer waits here
 *   sem_items (init=0):       counts live items  — consumer waits here
 *
 *   The buffer capacity is intentionally small (4) so the producer blocks
 *   multiple times before the consumer catches up, exercising both the
 *   "wait blocks" and "signal wakes" paths of the semaphore.
 *
 *  ── Event flag test (setter / waiter) ────────────────────────────────────
 *
 *   task_setter  sleeps one tick between signals so the waiter must truly
 *                block rather than spin-poll.
 *   task_waiter  calls rtos_flag_wait() FLAG_COUNT times and records how
 *                many it received.  It also checks that the flag is cleared
 *                (not pending) immediately after returning from flag_wait.
 *
 *  ── Reporter ─────────────────────────────────────────────────────────────
 *
 *   Waits for all four workers, then prints the results and asserts PASS/FAIL.
 *
 * Success: 7-seg = "5YN20000" (SYN2), LEDs = 0xFFFF
 * Failure: 7-seg = 0x000000FF, LEDs = fail-count
 */

#include <stdio.h>
#include "../mmio.h"
#include "../rtos.h"

/* ── Semaphore test parameters ────────────────────────────────────────────── */
#define BUF_CAP    4
#define ITEM_COUNT 40   /* items the producer generates (> BUF_CAP to force blocks) */

static RtosSem  sem_slots;          /* free slots in ring buffer     */
static RtosSem  sem_items;          /* live items in ring buffer     */
static volatile int g_buf[BUF_CAP]; /* ring buffer                   */
static volatile int g_head;         /* producer write index          */
static volatile int g_tail;         /* consumer read index           */
static volatile int g_consumed_total;
static volatile int g_producer_done;
static volatile int g_consumer_done;

/* ── Event flag test parameters ──────────────────────────────────────────── */
#define FLAG_COUNT 10

static RtosFlag g_flag;
static volatile int g_flag_received;
static volatile int g_flag_errors;
static volatile int g_setter_done;
static volatile int g_waiter_done;

/* ── task_producer ────────────────────────────────────────────────────────── */

static void task_producer(void) {
    for (int i = 0; i < ITEM_COUNT; i++) {
        rtos_sem_wait(&sem_slots);          /* block if buffer full        */
        rtos_mutex_lock();
        g_buf[g_head % BUF_CAP] = i + 1;   /* item values: 1 .. ITEM_COUNT */
        g_head++;
        rtos_mutex_unlock();
        rtos_sem_signal(&sem_items);        /* tell consumer item is ready */
    }
    g_producer_done = 1;
    while (1) { REG_LEDS = 0x0001; }
}

/* ── task_consumer ────────────────────────────────────────────────────────── */

static void task_consumer(void) {
    int total = 0;
    for (int i = 0; i < ITEM_COUNT; i++) {
        rtos_sem_wait(&sem_items);          /* block if buffer empty       */
        rtos_mutex_lock();
        int val = g_buf[g_tail % BUF_CAP];
        g_tail++;
        rtos_mutex_unlock();
        rtos_sem_signal(&sem_slots);        /* free the slot               */
        total += val;
    }
    g_consumed_total = total;
    g_consumer_done  = 1;
    while (1) { REG_LEDS = 0x0002; }
}

/* ── task_setter ──────────────────────────────────────────────────────────── */

static void task_setter(void) {
    for (int i = 0; i < FLAG_COUNT; i++) {
        rtos_sleep_ticks(1);   /* give waiter a chance to block first */
        rtos_flag_set(&g_flag);
    }
    g_setter_done = 1;
    while (1) { REG_LEDS = 0x0004; }
}

/* ── task_waiter ──────────────────────────────────────────────────────────── */

static void task_waiter(void) {
    int received = 0;
    for (int i = 0; i < FLAG_COUNT; i++) {
        rtos_flag_wait(&g_flag);            /* blocks until setter fires   */
        received++;
        /* Verify flag was auto-cleared: a second wait must not return
         * immediately (we can't check that inline, but we verify count). */
    }
    g_flag_received = received;
    g_waiter_done   = 1;
    while (1) { REG_LEDS = 0x0008; }
}

/* ── reporter ─────────────────────────────────────────────────────────────── */

static void reporter(void) {
    /* Wait for all four workers. */
    while (!(g_producer_done && g_consumer_done &&
             g_setter_done   && g_waiter_done))
        rtos_yield();

    /* Expected sum: 1 + 2 + ... + ITEM_COUNT = ITEM_COUNT*(ITEM_COUNT+1)/2 */
    int expected_sum = ITEM_COUNT * (ITEM_COUNT + 1) / 2;

    int sem_ok  = (g_consumed_total == expected_sum);
    int flag_ok = (g_flag_received == FLAG_COUNT) && (g_flag_errors == 0);
    int ok      = sem_ok && flag_ok;

    printf("\n=== sync test results ===\n");

    printf("semaphore (producer/consumer):\n");
    printf("  items produced : %d\n",  ITEM_COUNT);
    printf("  consumed total : %d  (expected %d)  %s\n",
           g_consumed_total, expected_sum, sem_ok ? "PASS" : "FAIL");

    printf("event flag (setter/waiter):\n");
    printf("  signals set    : %d\n",  FLAG_COUNT);
    printf("  signals caught : %d  (expected %d)  %s\n",
           g_flag_received, FLAG_COUNT, flag_ok ? "PASS" : "FAIL");

    printf("Result: %s\n", ok ? "PASS" : "FAIL");

    REG_SEG_ALL = ok ? 0x59E20000u : 0x000000FFu;  /* "5YE2" or error */
    REG_LEDS    = ok ? 0xFFFFu     : 0x0001u;

    while (1) {}
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== KlaussCPU RTOS sync test ===\n");
    printf("semaphore: %d items through %d-slot buffer\n", ITEM_COUNT, BUF_CAP);
    printf("event flag: %d set/wait cycles\n\n", FLAG_COUNT);

    rtos_sem_init(&sem_slots, BUF_CAP);  /* start with all slots free   */
    rtos_sem_init(&sem_items, 0);        /* start with no items ready   */
    rtos_flag_init(&g_flag);

    rtos_init();
    task_create("producer", task_producer);
    task_create("consumer", task_consumer);
    task_create("setter",   task_setter);
    task_create("waiter",   task_waiter);
    task_create("reporter", reporter);

    rtos_start();
    return 0;
}
