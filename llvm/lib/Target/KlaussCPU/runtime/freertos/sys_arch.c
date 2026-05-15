/*
 * sys_arch.c — lwIP OS abstraction implementation for KlaussCPU + FreeRTOS.
 *
 * Wraps FreeRTOS semaphores, queues, and tasks as lwIP's sys_* primitives.
 *
 * Timeout conventions (lwIP):
 *   timeout = 0          → wait forever
 *   timeout > 0          → wait at most that many milliseconds
 *   return SYS_ARCH_TIMEOUT if the wait expired without a message/signal.
 *   return ms elapsed    if successful (approximate, tick-granularity).
 */

#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/err.h"
#include "arch/sys_arch.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"

#include "mmio.h"   /* REG_CLOCK_MS */

/* ── sys_init ───────────────────────────────────────────────────────────── */

void sys_init(void) {
    /* FreeRTOS is already initialised before lwIP starts. Nothing to do. */
}

/* ── sys_now ────────────────────────────────────────────────────────────── */
/*
 * Returns milliseconds since some epoch.  lwIP uses this for timeout
 * tracking in ARP, TCP, DHCP, DNS, SNTP, etc.
 * REG_CLOCK_MS is a free-running 64-bit ms counter driven by the 100 MHz
 * clock; we take the low 32 bits (wraps after ~49 days).
 */
u32_t sys_now(void) {
    return (u32_t)REG_CLOCK_MS;
}

/* ── Semaphores ──────────────────────────────────────────────────────────── */

err_t sys_sem_new(sys_sem_t *sem, u8_t count) {
    *sem = xSemaphoreCreateCounting(255, (UBaseType_t)count);
    return (*sem != NULL) ? ERR_OK : ERR_MEM;
}

void sys_sem_signal(sys_sem_t *sem) {
    xSemaphoreGive(*sem);
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout) {
    TickType_t start = xTaskGetTickCount();
    TickType_t ticks = (timeout == 0) ? portMAX_DELAY
                                      : pdMS_TO_TICKS(timeout);
    if (xSemaphoreTake(*sem, ticks) == pdTRUE) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        return (u32_t)(elapsed * portTICK_PERIOD_MS);
    }
    return SYS_ARCH_TIMEOUT;
}

void sys_sem_free(sys_sem_t *sem) {
    vSemaphoreDelete(*sem);
    *sem = NULL;
}

/* ── Mutexes ─────────────────────────────────────────────────────────────── */

err_t sys_mutex_new(sys_mutex_t *mutex) {
    *mutex = xSemaphoreCreateMutex();
    return (*mutex != NULL) ? ERR_OK : ERR_MEM;
}

void sys_mutex_lock(sys_mutex_t *mutex) {
    xSemaphoreTake(*mutex, portMAX_DELAY);
}

void sys_mutex_unlock(sys_mutex_t *mutex) {
    xSemaphoreGive(*mutex);
}

void sys_mutex_free(sys_mutex_t *mutex) {
    vSemaphoreDelete(*mutex);
    *mutex = NULL;
}

/* ── Mailboxes (message queues of void *) ───────────────────────────────── */

err_t sys_mbox_new(sys_mbox_t *mbox, int size) {
    *mbox = xQueueCreate((UBaseType_t)size, sizeof(void *));
    return (*mbox != NULL) ? ERR_OK : ERR_MEM;
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg) {
    /* Block until space available — should rarely stall in practice. */
    xQueueSend(*mbox, &msg, portMAX_DELAY);
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg) {
    return (xQueueSend(*mbox, &msg, 0) == pdTRUE) ? ERR_OK : ERR_MEM;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    err_t rc = (xQueueSendFromISR(*mbox, &msg, &xHigherPriorityTaskWoken)
                == pdTRUE) ? ERR_OK : ERR_MEM;
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    return rc;
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout) {
    void *dummy;
    if (msg == NULL) msg = &dummy;

    TickType_t start = xTaskGetTickCount();
    TickType_t ticks = (timeout == 0) ? portMAX_DELAY
                                      : pdMS_TO_TICKS(timeout);
    if (xQueueReceive(*mbox, msg, ticks) == pdTRUE) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        return (u32_t)(elapsed * portTICK_PERIOD_MS);
    }
    *msg = NULL;
    return SYS_ARCH_TIMEOUT;
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg) {
    void *dummy;
    if (msg == NULL) msg = &dummy;
    if (xQueueReceive(*mbox, msg, 0) == pdTRUE)
        return 0;
    *msg = NULL;
    return SYS_MBOX_EMPTY;
}

void sys_mbox_free(sys_mbox_t *mbox) {
    vQueueDelete(*mbox);
    *mbox = NULL;
}

/* ── Threads ─────────────────────────────────────────────────────────────── */

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn fn,
                             void *arg, int stacksize, int prio) {
    TaskHandle_t h = NULL;
    /* xTaskCreate takes depth in StackType_t words.
     * lwipopts.h defines TCPIP_THREAD_STACKSIZE / DEFAULT_THREAD_STACKSIZE
     * in WORDS (the FreeRTOS convention used by all ARM ports) so we pass
     * the value straight through — no byte→word division. */
    uint32_t words = (uint32_t)stacksize;
    if (words < configMINIMAL_STACK_SIZE)
        words = configMINIMAL_STACK_SIZE;
    xTaskCreate(fn, name, (uint16_t)words, arg, (UBaseType_t)prio, &h);
    return h;
}
