/*
 * portmacro.h — FreeRTOS port definitions for KlaussCPU.
 *
 * KlaussCPU data model: int=32-bit, long/void*=64-bit, addresses 32-bit.
 *
 * Interrupt model: single interrupt source (timer, source 0) controlled by
 * REG_INT_MASK (MMIO 0xF00F_0000).  Write 0 = disable, write 1 = enable.
 * Hardware auto-clears INT_MASK[0] on dispatch; IRET restores it from the
 * saved frame.  This gives free critical-section semantics: disabling the
 * timer is sufficient for mutual exclusion on a single-core CPU.
 *
 * Stack: 8-byte slots, grows downward.  StackType_t = uint64_t matches the
 * hardware push/pop unit (PUSH/POP are always 8-byte operations).
 */

#ifndef PORTMACRO_H
#define PORTMACRO_H

#include <stdint.h>
#include "mmio.h"   /* REG_INT_MASK */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Type definitions ──────────────────────────────────────────────────── */

#define portSTACK_TYPE          uint64_t
typedef portSTACK_TYPE          StackType_t;    /* 8-byte hardware stack slot */
typedef long                    BaseType_t;     /* 64-bit signed */
typedef unsigned long           UBaseType_t;    /* 64-bit unsigned */
typedef uint32_t                TickType_t;
#define portMAX_DELAY           ((TickType_t)0xFFFFFFFFUL)
#define portTICK_TYPE_IS_ATOMIC 1

/* ── Stack ──────────────────────────────────────────────────────────────── */

#define portSTACK_GROWTH        (-1)    /* grows downward */
#define portBYTE_ALIGNMENT      8       /* StackType_t is 8 bytes */

/* ── Tick ───────────────────────────────────────────────────────────────── */

#define portTICK_PERIOD_MS      ((TickType_t)1000 / (TickType_t)configTICK_RATE_HZ)

/* ── Critical section ───────────────────────────────────────────────────── */
/*
 * Nesting counter for portENTER_CRITICAL / portEXIT_CRITICAL.
 * portDISABLE/ENABLE_INTERRUPTS are unnested (raw mask writes) and are
 * only called from scheduler internals where nesting is guaranteed not to
 * matter.
 */
extern uint32_t uxPortCriticalNesting;

#define portDISABLE_INTERRUPTS()    do { REG_INT_MASK = 0; } while (0)
#define portENABLE_INTERRUPTS()     do { REG_INT_MASK = 1; } while (0)

void vPortEnterCritical(void);
void vPortExitCritical(void);
#define portENTER_CRITICAL()        vPortEnterCritical()
#define portEXIT_CRITICAL()         vPortExitCritical()

/*
 * ISR-safe critical section (used by queue/semaphore routines called from
 * ISRs).  Our timer ISR auto-disables the timer on entry, so nesting is
 * not possible.  Return/restore the previous INT_MASK value.
 */
#define portSET_INTERRUPT_MASK_FROM_ISR()   \
    ({ uint32_t _m = REG_INT_MASK; REG_INT_MASK = 0; (UBaseType_t)_m; })
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(x) \
    do { REG_INT_MASK = (uint32_t)(x); } while (0)

/* ── Yield ──────────────────────────────────────────────────────────────── */

void vPortYield(void);
#define portYIELD()                 vPortYield()

/*
 * portYIELD_FROM_ISR: called at the end of a FreeRTOS ISR when a higher-
 * priority task has been unblocked.  On KlaussCPU the tick ISR handles
 * context switching internally, so a yield from other ISR sources (not yet
 * used) would need to set a flag and let the tick ISR act on it.  For now
 * this is a no-op since the tick ISR already handles all switches.
 */
#define portYIELD_FROM_ISR(x)       do { (void)(x); } while (0)

/* ── Task function ──────────────────────────────────────────────────────── */

#define portTASK_FUNCTION_PROTO(vFunc, pvParams)    void vFunc(void *pvParams)
#define portTASK_FUNCTION(vFunc, pvParams)          void vFunc(void *pvParams)

/* ── Pointer size type ──────────────────────────────────────────────────── */
/*
 * Used by FreeRTOS internals for pointer ↔ integer conversions (e.g.
 * alignment masking in prvInitialiseNewTask).  Pointers on KlaussCPU are
 * 64-bit values, so use uint64_t to avoid truncation warnings.
 */
#define portPOINTER_SIZE_TYPE   uint64_t

/* ── Misc ───────────────────────────────────────────────────────────────── */

#define portNOP()               __asm__ volatile("nop")
#define portINLINE              __inline__
#define portFORCE_INLINE        __attribute__((always_inline))
#define portMEMORY_BARRIER()    __asm__ volatile("" ::: "memory")
#define portUNUSED(x)           ((void)(x))

#ifdef __cplusplus
}
#endif

#endif /* PORTMACRO_H */
