/*
 * fatal.c — KlaussCPU fatal error handler for Zephyr.
 */

#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/fatal.h>

void z_klausscpu_fatal_error(unsigned int reason, const z_arch_esf_t *esf)
{
    z_fatal_error(reason, esf);
}

void arch_system_halt(unsigned int reason)
{
    ARG_UNUSED(reason);
    /* Disable all interrupts and halt. */
    __asm__ volatile(
        "setr   r0, 0xF00F0000\n\t"
        "setr   r1, 0\n\t"
        "memset32 r1, r0\n\t"
        : : : "r0", "r1", "memory"
    );
    __builtin_trap();
}
