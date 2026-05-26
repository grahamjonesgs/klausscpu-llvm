#ifndef ZEPHYR_INCLUDE_ARCH_KLAUSSCPU_ARCH_H_
#define ZEPHYR_INCLUDE_ARCH_KLAUSSCPU_ARCH_H_

/*
 * KlaussCPU architecture interface for Zephyr.
 *
 * Physical address space: 32-bit (128 MiB RAM + MMIO above 0xF000_0000).
 * GPRs: R0-R15, 64-bit.  Stack slots: 8 bytes.  Stack: grows down.
 */

#include <zephyr/arch/common/sys_bitops.h>
#include <zephyr/arch/common/ffs.h>
#include <zephyr/arch/common/addr_types.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>

/* Minimum stack pointer alignment: 8 bytes (one GPR slot). */
#define ARCH_STACK_PTR_ALIGN  8

/*
 * Thread stack reservation: 0.
 *
 * arch_new_thread() receives stack_ptr = top (the full stack top) and builds
 * the initial frame (16 GPRs + IRET = 136 bytes) downward from there.
 * A non-zero reservation would cause Zephyr to pass stack_ptr = top-N, so
 * arch_new_thread would write the frame to top-N-136 — overflowing below the
 * stack for small stacks (e.g. the 256-byte idle stack would overflow by 16 B).
 */
#define ARCH_THREAD_STACK_RESERVED  0

/*
 * arch_esf — exception stack frame saved by the timer ISR.
 * Contains all 16 GPRs + the hardware IRET word.
 */
struct arch_esf {
    uint64_t r[16];    /* R0..R15 */
    uint64_t iret;     /* hardware IRET frame (INT_MASK | flags | PC) */
};

/* No user-mode or syscall support. */
#define ARCH_SYSCALL_LIMIT  0

/*
 * arch_nop — NOP instruction.
 */
static ALWAYS_INLINE void arch_nop(void)
{
    __asm__ volatile("nop");
}

/* MMIO interrupt-controller base. */
#define _KLAUSSCPU_INT_MASK (*(volatile uint32_t *)0xF00F0000u)

/*
 * arch_irq_lock / arch_irq_unlock — disable/enable all interrupts.
 * Atomically reads the current mask, writes 0, returns old mask.
 */
static ALWAYS_INLINE unsigned int arch_irq_lock(void)
{
    unsigned int key = _KLAUSSCPU_INT_MASK;
    _KLAUSSCPU_INT_MASK = 0u;
    return key;
}

static ALWAYS_INLINE void arch_irq_unlock(unsigned int key)
{
    _KLAUSSCPU_INT_MASK = key;
}

static ALWAYS_INLINE bool arch_irq_unlocked(unsigned int key)
{
    return key != 0u;
}

/* Hardware cycle counter at MMIO 0xF00F0038 (32-bit, runs at SYS_CLOCK_HW_CYCLES_PER_SEC). */
#define _KLAUSSCPU_TIMER_CNT (*(volatile uint32_t *)0xF00F0038u)

static ALWAYS_INLINE uint32_t arch_k_cycle_get_32(void)
{
    return _KLAUSSCPU_TIMER_CNT;
}

static ALWAYS_INLINE uint64_t arch_k_cycle_get_64(void)
{
    return (uint64_t)_KLAUSSCPU_TIMER_CNT;
}

/* arch_switch is in swap.S; declared via arch_interface.h */

#include <kernel_arch_func.h>

#endif /* ZEPHYR_INCLUDE_ARCH_KLAUSSCPU_ARCH_H_ */
