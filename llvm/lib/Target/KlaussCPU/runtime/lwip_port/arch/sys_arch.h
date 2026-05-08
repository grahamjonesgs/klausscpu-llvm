// arch/sys_arch.h — lwIP OS abstraction stub for NO_SYS=1.
//
// With NO_SYS=1 lwIP needs no threads, semaphores, or mailboxes.
// The only OS primitive required is sys_now() (provided in sys_arch.c).
// Critical-section protection uses the RTOS interrupt mask.

#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

#include <stdint.h>
#include "../../mmio.h"   // REG_INT_MASK, INTMASK_ALL

// SYS_LIGHTWEIGHT_PROT=1: lwIP calls sys_arch_protect/unprotect around
// shared data structures.  On a single-core CPU, disabling the timer
// interrupt is sufficient mutual exclusion.
typedef unsigned int sys_prot_t;

static inline sys_prot_t sys_arch_protect(void) {
    REG_INT_MASK = 0;   // disable all interrupts
    return 0;
}
static inline void sys_arch_unprotect(sys_prot_t pval) {
    (void)pval;
    REG_INT_MASK = INTMASK_ALL;   // re-enable (timer, and ETH once wired)
}

#endif // LWIP_ARCH_SYS_ARCH_H
