/*
 * irq.c — KlaussCPU IRQ management for Zephyr.
 *
 * KlaussCPU has a flat MMIO interrupt controller:
 *   REG_INT_MASK  (0xF00F_0000): write 0 = all off, write 1 = timer on,
 *                                write 3 = timer+ethernet on.
 *   REG_INT_VEC(n): 32-bit function pointer for source n.
 *
 * Zephyr treats IRQ numbers as small integers (0 = timer, 1 = ethernet).
 * We maintain a per-source enable bitmask and write the whole mask each time.
 */

#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/irq.h>
#include <zephyr/init.h>

/* Declared in zephyr/lib/libc/minimal/source/stdout/stdout_console.c.
 * Not exposed via any public header, so forward-declare here exactly
 * as the other console drivers (uart_console.c, ram_console.c, …) do. */
extern void __stdout_hook_install(int (*hook)(int c));

/* MMIO shortcuts (no mmio.h dependency to keep arch self-contained). */
#define _INT_MASK   (*(volatile uint32_t *)0xF00F0000u)
#define _INT_PEND   (*(volatile uint32_t *)0xF00F0008u)
#define _INT_VEC(n) (*(volatile uint32_t *)(0xF00F0010u + 8u * (n)))

/* Software bitmask of currently-enabled interrupt sources. */
static uint32_t _irq_enabled_mask;

/* "In ISR" depth counter — read by arch_is_in_isr(). */
volatile uint32_t _klausscpu_in_isr;

/* Kernel stack for ISR use — sized generously for sys_clock_isr() depth. */
#define KERNEL_STACK_WORDS 512
static uint64_t _kernel_stack[KERNEL_STACK_WORDS];
uint32_t _klausscpu_kernel_sp; /* read from swap.S */

void arch_irq_enable(unsigned int irq)
{
    _irq_enabled_mask |= (1u << irq);
    _INT_MASK = _irq_enabled_mask;
}

void arch_irq_disable(unsigned int irq)
{
    _irq_enabled_mask &= ~(1u << irq);
    _INT_MASK = _irq_enabled_mask;
}

int arch_irq_is_enabled(unsigned int irq)
{
    return (_irq_enabled_mask & (1u << irq)) != 0u;
}

void z_irq_spurious(const void *unused)
{
    ARG_UNUSED(unused);
    /* Nothing to do; the controller auto-clears pending on IRET. */
}

/*
 * arch_irq_connect_dynamic — install a handler for the given IRQ source.
 * flags is ignored (no priority or trigger config on this controller).
 */
int arch_irq_connect_dynamic(unsigned int irq, unsigned int priority,
                              void (*routine)(const void *), const void *parameter,
                              uint32_t flags)
{
    ARG_UNUSED(priority);
    ARG_UNUSED(flags);
    _INT_VEC(irq) = (uint32_t)(uintptr_t)routine;
    return irq;
}

/*
 * Called once by the timer driver before enabling the timer IRQ.
 * Sets up the kernel stack pointer used by the ISR.
 */
void klausscpu_irq_init(void)
{
    _klausscpu_kernel_sp =
        (uint32_t)(uintptr_t)(_kernel_stack + KERNEL_STACK_WORDS);
}

/*
 * Override the weak default in zephyr/lib/os/printk.c so printk() actually
 * routes characters to our UART instead of being silently discarded.
 *
 * Zephyr normally wires this up via drivers/console/uart_console.c, which
 * calls __printk_hook_install(console_out) at SYS_INIT.  That driver isn't
 * being compiled in this build (no CONFIG_*_CONSOLE_SUBSYS dependency met),
 * so _char_out stays pointing at the weak no-op arch_printk_char_out.  By
 * supplying a strong arch_printk_char_out here we make printk work without
 * needing the full Zephyr console subsystem.
 *
 * Uses the same TXCHARMEMR builtin path as our UART driver — works from any
 * context, no kernel state required.
 */
static volatile char _printk_char_buf;

int arch_printk_char_out(int c)
{
    if (c == '\n') {
        _printk_char_buf = '\r';
        __builtin_klausscpu_txcharmemr((const void *)&_printk_char_buf);
    }
    _printk_char_buf = (char)c;
    __builtin_klausscpu_txcharmemr((const void *)&_printk_char_buf);
    return c;
}

/*
 * Route minimal-libc stdout (used by printf/fprintf/puts when CONFIG_MINIMAL_LIBC=y
 * and CONFIG_STDOUT_CONSOLE=y) through the same UART path as printk.
 *
 * Zephyr's drivers/console/uart_console.c normally calls __stdout_hook_install()
 * at SYS_INIT, but that driver isn't being compiled in this build (no
 * matching CONSOLE_SUBSYS Kconfig), so without this hook the libc default
 * _stdout_hook returns EOF for every byte and printf produces no output.
 */
static int _klausscpu_stdout_install(void)
{
    __stdout_hook_install(arch_printk_char_out);
    return 0;
}

SYS_INIT(_klausscpu_stdout_install, PRE_KERNEL_1, 60);
