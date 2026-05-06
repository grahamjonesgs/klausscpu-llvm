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

    for (char *p = __bss_start; p != __bss_end; ++p)
        *p = 0;

    /* Write heap start to the hardware heap-header slot at address 0. */
    *(volatile unsigned long long *)0 = (unsigned long long)(char *)_end;

    /* Software startup delay — replaces the removed DELAYV hardware opcode.
     * Gives the UART receiver time to be ready before the first output.
     * Loop count matches the old DELAYV 0x500 intent; each iteration takes
     * several cycles so total is similar to the original hardware delay. */
    for (volatile unsigned int i = 0; i < 5000; i++)
        ;

    __stdio_init();
    main(0, (char **)0);

    __builtin_trap();
}
