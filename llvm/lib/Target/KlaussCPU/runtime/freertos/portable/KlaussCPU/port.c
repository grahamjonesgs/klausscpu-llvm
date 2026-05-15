/*
 * port.c — FreeRTOS KlaussCPU port (C layer).
 *
 * Implements the three functions the FreeRTOS kernel requires:
 *   pxPortInitialiseStack  — build a fake context frame for a new task
 *   xPortStartScheduler    — install ISR, start timer, launch first task
 *   vPortEndScheduler      — disable timer (rarely called)
 *
 * Plus the critical-section helpers exposed by portmacro.h.
 *
 * Hardware context frame layout (from CPU_ARCHITECTURE.md §13.1):
 *   bits [63:43] = 0
 *   bits [42:39] = INT_MASK (4 bits, restored by IRET)
 *   bits [38:32] = processor flags (7 bits)
 *   bits [31: 0] = PC
 *
 * Full per-task stack frame (low address → high address):
 *   R15  ← SP saved here (timer_handler pops this first)
 *   R14
 *   ...
 *   R1
 *   R0   ← pvParameters (ABI arg0)
 *   hw_frame  ← IRET pops this last
 *
 * Each slot is 8 bytes (StackType_t = uint64_t).
 */

#include "FreeRTOS.h"
#include "task.h"
#include "portmacro.h"
#include "mmio.h"

/* ── Kernel stack ──────────────────────────────────────────────────────── */
/*
 * The timer ISR switches to this stack before calling xTaskIncrementTick()
 * and vTaskSwitchContext().  Keeps scheduler C code off the interrupted
 * task's stack (avoids corrupting the frame being built there).
 */
#define PORT_KERNEL_STACK_WORDS  256
static StackType_t xKernelStack[PORT_KERNEL_STACK_WORDS];
uint32_t g_freertos_kernel_sp;  /* read by port.S — must be uint32_t */

/* ── Critical-section nesting counter ─────────────────────────────────── */
uint32_t uxPortCriticalNesting = 0;

/* ── pxPortInitialiseStack ─────────────────────────────────────────────── */

StackType_t *pxPortInitialiseStack(StackType_t *pxTopOfStack,
                                    TaskFunction_t pxCode,
                                    void *pvParameters) {
    /*
     * pxTopOfStack points to the highest valid address in the allocated stack
     * buffer.  We fill downward and return the new SP (lowest address of the
     * constructed frame).
     *
     * timer_handler push order: push r0, push r1, ..., push r15.
     * So in memory (from current SP upward): r15, r14, ..., r1, r0, hw_frame.
     * pop order reverses: pop r15 (first pop), ..., pop r0 (last pop), then IRET.
     *
     * We build from pxTopOfStack downward to match this layout.
     */

    /* Hardware IRET frame: INT_MASK[0]=1 so timer fires immediately on task
       entry; flags=0; PC = task entry function. */
    *pxTopOfStack = ((uint64_t)1u << 39)           /* INT_MASK[0] = 1 */
                  | (uint64_t)(uint32_t)pxCode;     /* PC = task entry */
    pxTopOfStack--;

    /* R0 = pvParameters (first argument register per ABI). */
    *pxTopOfStack = (uint64_t)(uint32_t)pvParameters;
    pxTopOfStack--;

    /* R1 through R14 = 0. */
    for (int i = 0; i < 14; i++) {
        *pxTopOfStack = 0;
        pxTopOfStack--;
    }

    /* R15 = 0 (frame pointer — no caller frame).
       This is the slot timer_handler pops first; SP is saved pointing here. */
    *pxTopOfStack = 0;

    return pxTopOfStack;
}

/* ── xPortStartScheduler ───────────────────────────────────────────────── */

extern void vPortStartFirstTask(void);  /* port.S */
extern void vPortTickISR(void);         /* port.S — installed as INT_VEC0 */

BaseType_t xPortStartScheduler(void) {
    /* Compute and store kernel stack top for the timer ISR. */
    g_freertos_kernel_sp =
        (uint32_t)(unsigned long)(xKernelStack + PORT_KERNEL_STACK_WORDS);

    /* Install timer ISR and set tick period.
       Timer period = CPU_CLOCK_HZ / TICK_RATE_HZ cycles.
       Writing TIMER_PER resets the counter, so the first tick fires after
       exactly one period. */
    REG_INT_VEC(0) = (uint32_t)(unsigned long)vPortTickISR;
    REG_TIMER_PER  = (uint32_t)(configCPU_CLOCK_HZ / configTICK_RATE_HZ);

    /* Switch to the first task's stack, restore its fake register frame,
       enable the timer, and IRET into its entry function.  Never returns. */
    vPortStartFirstTask();

    __builtin_unreachable();
    return pdFALSE;
}

void vPortEndScheduler(void) {
    REG_INT_MASK = 0;
}

/* ── Critical section ──────────────────────────────────────────────────── */

void vPortEnterCritical(void) {
    REG_INT_MASK = 0;
    uxPortCriticalNesting++;
}

void vPortExitCritical(void) {
    if (uxPortCriticalNesting > 0) {
        uxPortCriticalNesting--;
        if (uxPortCriticalNesting == 0)
            REG_INT_MASK = 1;
    }
}

/* ── Application hooks (weak — override in application code) ───────────── */

void __attribute__((weak)) vApplicationIdleHook(void) {}

void __attribute__((weak)) vApplicationMallocFailedHook(void) {
    for (;;) {}
}

void __attribute__((weak)) vApplicationStackOverflowHook(
        TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask;
    /* pcTaskName may be corrupted if overflow was severe, but usually readable. */
    extern int printf(const char *, ...);
    printf("STACK OVERFLOW: task '%s'\n", pcTaskName ? pcTaskName : "?");
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

void __attribute__((weak)) vApplicationTickHook(void) {}
