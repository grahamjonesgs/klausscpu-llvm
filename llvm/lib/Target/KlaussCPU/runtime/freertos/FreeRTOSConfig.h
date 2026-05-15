/*
 * FreeRTOSConfig.h — KlaussCPU FreeRTOS configuration.
 *
 * Tune these values to match your application's memory budget and
 * real-time requirements.  Defaults are conservative and correct.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ── Scheduler behaviour ────────────────────────────────────────────────── */

#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_TICKLESS_IDLE                 0   /* no low-power mode */
#define configUSE_IDLE_HOOK                     1
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            1
#define configCHECK_FOR_STACK_OVERFLOW          2   /* full stack-paint check */

/* ── Clock ──────────────────────────────────────────────────────────────── */

#define configCPU_CLOCK_HZ                      100000000UL  /* 100 MHz */
#define configTICK_RATE_HZ                      ((TickType_t)1000)  /* 1 ms */
/* TickType_t = uint32_t; required by FreeRTOS V11+ */
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS

/* ── Task priorities ────────────────────────────────────────────────────── */

#define configMAX_PRIORITIES                    8
#define configIDLE_TASK_NAME                    "IDLE"

/* ── Stack sizes (in StackType_t = 8-byte units) ───────────────────────── */
/*
 * MINIMAL_STACK_SIZE is used for the idle task.  Tasks that call printf
 * (picolibc stdio) or use significant recursion need at least 512 words
 * (4 KB).  The idle task does neither so 128 words (1 KB) suffices.
 */
#define configMINIMAL_STACK_SIZE                ((uint16_t)128)
#define configSTACK_DEPTH_TYPE                  uint16_t

/* ── Heap ───────────────────────────────────────────────────────────────── */
/*
 * heap_4.c is recommended: it coalesces freed blocks and handles arbitrary
 * alloc/free patterns without fragmentation for typical embedded use.
 * 256 KB is comfortable for ~8 tasks with 4 KB stacks + queue overhead.
 */
#define configTOTAL_HEAP_SIZE                   ((size_t)(256 * 1024))

/* ── Queues / synchronisation ───────────────────────────────────────────── */

#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_QUEUE_SETS                    0
#define configQUEUE_REGISTRY_SIZE               8

/* ── Software timers ────────────────────────────────────────────────────── */

#define configUSE_TIMERS                        0   /* enable when needed */
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            configMINIMAL_STACK_SIZE

/* ── Task selection ─────────────────────────────────────────────────────── */
/*
 * Port-optimised selection uses hardware CLZ to find the highest-priority
 * ready task in O(1).  Set to 1 once a CLZ intrinsic is wired up.
 */
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0

/* ── Tracing / statistics ───────────────────────────────────────────────── */

#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

/* ── Task notification ──────────────────────────────────────────────────── */

#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   1

/* ── Co-routines (deprecated) ───────────────────────────────────────────── */

#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         1

/* ── API inclusion ──────────────────────────────────────────────────────── */

#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xResumeFromISR                  1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xEventGroupSetBitFromISR        0
#define INCLUDE_xTimerPendFunctionCall          0
#define INCLUDE_xTaskAbortDelay                 1

/* ── Assert ─────────────────────────────────────────────────────────────── */

extern void vAssertCalled(const char *file, unsigned long line);
#define configASSERT(x) \
    do { if (!(x)) vAssertCalled(__FILE__, __LINE__); } while (0)

#endif /* FREERTOS_CONFIG_H */
