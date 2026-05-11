/*
 * crt0.c — KlaussCPU minimal C runtime startup
 *
 * STACK INITIALISATION NOTE
 * ─────────────────────────
 * KlaussCPU has no hardware reset vector that auto-loads SP.  The linker
 * script defines _stack_top = 0x08000000 (top of 128 MiB RAM).
 *
 * Option A — hardware sets SP before _start runs:
 *   Configure the Verilog reset logic to set SP = 0x08000000.  No changes
 *   needed here.
 *
 * Option B — software SP init (requires hardware to provide a *sane* initial
 *   SP so that _start's prologue (PUSH R15) does not corrupt arbitrary memory):
 *   Enable the __builtin_stack_restore call below.  The prologue will briefly
 *   use whatever SP the hardware provided, then the body resets SP to _stack_top.
 *   This is safe as long as the hardware's initial SP is within RAM.
 *
 * Encoding reference for SP init (in case a naked stub is needed later):
 *   SETR  r0, 0x08000000 → bytes 00 00 08 00  08 00 00 00   (8 bytes)
 *   SETSP r0             → bytes 00 00 40 40               (4 bytes)
 */

/* Debug LED checkpoints: visible on the board, narrow down crash phase.
 * Each write is a different nibble so a glance at the LEDs tells you where
 * startup got to.  Defined via direct MMIO so crt0 needs no extra includes. */
#define _CRT_LEDS  (*(volatile unsigned int *)0xF0040000u)

extern int main(int argc, char **argv);
extern void __stdio_init(void);

/* Linker-script symbols marking the BSS region */
extern char __bss_start[], __bss_end[], _end[];

/* _stack_top: the address of this symbol IS the stack-top value (0x08000000). */
extern char _stack_top[];

__attribute__((noreturn, used))
void _start(void) {

#ifdef KLAUSSCPU_SW_STACK_INIT
    __builtin_stack_restore((void *)_stack_top);
#endif

    _CRT_LEDS = 0x1111;   /* checkpoint A: entered _start */

    for (char *p = __bss_start; p != __bss_end; ++p)
        *p = 0;

    _CRT_LEDS = 0x2222;   /* checkpoint B: BSS clear done */

    /* Write heap start to the hardware heap-header slot at address 0. */
    *(volatile unsigned long long *)0 = (unsigned long long)(char *)_end;

    _CRT_LEDS = 0x3333;   /* checkpoint C: heap header written */

    /* Software startup delay — gives UART receiver time to be ready. */
    for (volatile unsigned int i = 0; i < 5000; i++)
        ;

    _CRT_LEDS = 0x4444;   /* checkpoint D: delay done */

    __stdio_init();

    _CRT_LEDS = 0x0000;   /* checkpoint E: stdio init done, calling main */

    main(0, (char **)0);

    __builtin_trap();
}
