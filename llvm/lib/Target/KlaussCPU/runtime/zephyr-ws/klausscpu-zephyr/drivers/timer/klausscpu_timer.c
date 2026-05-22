/*
 * klausscpu_timer.c — Zephyr sys_clock driver for KlaussCPU.
 *
 * Uses the MMIO timer at 0xF00F_0000 (same as the FreeRTOS port).
 * The timer ISR (z_klausscpu_timer_isr) lives in swap.S and calls
 * sys_clock_isr() from there; we just need to configure the hardware.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/irq.h>

/* MMIO register shortcuts. */
#define REG_INT_VEC(n)  (*(volatile uint32_t *)(0xF00F0010u + 8u * (n)))
#define REG_TIMER_PER   (*(volatile uint32_t *)0xF00F0030u)
#define REG_TIMER_CNT   (*(volatile uint32_t *)0xF00F0038u)

/* Declared in swap.S — the actual ISR entry point. */
extern void z_klausscpu_timer_isr(void);

/* Declared in irq.c — sets up the kernel stack pointer. */
extern void klausscpu_irq_init(void);

static int klausscpu_timer_init(void)
{
    klausscpu_irq_init();

    /* Install ISR at vector 0 (timer source). */
    REG_INT_VEC(0) = (uint32_t)(uintptr_t)z_klausscpu_timer_isr;

    /* Set timer period: CPU_HZ / TICKS_PER_SEC cycles. */
    REG_TIMER_PER = CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC
                  / CONFIG_SYS_CLOCK_TICKS_PER_SEC;

    /* Enable timer interrupt (source 0). */
    arch_irq_enable(0);

    return 0;
}

void sys_clock_set_timeout(int32_t ticks, bool idle)
{
    ARG_UNUSED(ticks);
    ARG_UNUSED(idle);
    /* Fixed-rate tick — no tickless support needed for hello_world. */
}

uint32_t sys_clock_elapsed(void)
{
    return 0;
}

uint32_t sys_clock_cycle_get_32(void)
{
    return REG_TIMER_CNT;
}

/* Called from swap.S timer ISR entry point — announce one tick to the kernel. */
void sys_clock_isr(void)
{
    sys_clock_announce(1);
}

/* Priority 2 = CONFIG_SYSTEM_CLOCK_INIT_PRIORITY default. */
SYS_INIT(klausscpu_timer_init, PRE_KERNEL_2, 2);
