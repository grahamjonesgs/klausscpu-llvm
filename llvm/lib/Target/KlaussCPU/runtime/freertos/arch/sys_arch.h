/*
 * arch/sys_arch.h — lwIP OS abstraction types for KlaussCPU + FreeRTOS.
 *
 * Defines sys_mutex_t, sys_sem_t, sys_mbox_t, sys_thread_t as thin wrappers
 * around FreeRTOS handle types.  lwIP includes this via "arch/sys_arch.h".
 *
 * SYS_ARCH_PROTECT/UNPROTECT use vPortEnterCritical/vPortExitCritical so
 * they work safely inside both task and ISR contexts.
 */

#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

#include "FreeRTOS.h"
#include "semphr.h"
#include "queue.h"
#include "task.h"

/* ── Core types ─────────────────────────────────────────────────────────── */

typedef SemaphoreHandle_t   sys_mutex_t;
typedef SemaphoreHandle_t   sys_sem_t;
typedef QueueHandle_t       sys_mbox_t;
typedef TaskHandle_t        sys_thread_t;

/* ── Validity helpers (lwIP calls these to check uninitialised handles) ─── */

#define sys_mutex_valid(m)          (*(m) != NULL)
#define sys_mutex_set_invalid(m)    do { *(m) = NULL; } while (0)

#define sys_sem_valid(s)            (*(s) != NULL)
#define sys_sem_set_invalid(s)      do { *(s) = NULL; } while (0)

#define sys_mbox_valid(b)           (*(b) != NULL)
#define sys_mbox_set_invalid(b)     do { *(b) = NULL; } while (0)

/* ── Lightweight protection (SYS_LIGHTWEIGHT_PROT=1) ───────────────────── */
/*
 * lwIP calls SYS_ARCH_PROTECT(lev) / SYS_ARCH_UNPROTECT(lev) around
 * accesses to shared data structures (ARP cache, timers, etc.).
 * We use the FreeRTOS critical section which disables the timer ISR.
 */
typedef unsigned int sys_prot_t;

#define SYS_ARCH_DECL_PROTECT(lev)  sys_prot_t lev
#define SYS_ARCH_PROTECT(lev)       do { taskENTER_CRITICAL(); (lev) = 1; } while (0)
#define SYS_ARCH_UNPROTECT(lev)     do { (void)(lev); taskEXIT_CRITICAL(); } while (0)

#endif /* LWIP_ARCH_SYS_ARCH_H */
