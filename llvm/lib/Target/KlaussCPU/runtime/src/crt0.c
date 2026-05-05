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

/* Linker-script symbols marking the BSS region */
extern char __bss_start[], __bss_end[], _end[];

/* _stack_top: the address of this symbol IS the stack-top value (0x08000000). */
extern char _stack_top[];

/* Heap top pointer owned by libc.c; initialised here so the compiler in
 * libc.c cannot constant-fold it (cross-TU write prevents the optimisation
 * that was replacing the scan base with literal 0). */
extern char *g_heap_top;

__attribute__((noreturn, used))
void _start(void) {

#ifdef KLAUSSCPU_SW_STACK_INIT
    /*
     * Option B: reset SP to the top of RAM.
     * Requires hardware to have set SP to some valid RAM address before
     * _start's prologue ran (so the PUSH R15 in the prologue was safe).
     * After this call SP = 0x08000000 and the remaining stack is fully usable.
     */
    __builtin_stack_restore((void *)_stack_top);
#endif

    /* Signal that _start has been reached. */
    __builtin_klausscpu_leds(0xAAAA);

    /* Hardware spin-wait — allow UART and peripherals to settle. */
    __builtin_klausscpu_delayv(0x500);

    /* Checkpoint: about to clear BSS. */
    __builtin_klausscpu_leds(0xBBBB);

    /* Zero-fill BSS */
    for (char *p = __bss_start; p != __bss_end; ++p)
        *p = 0;

    /* Initialise heap: must happen after BSS clear (which zeroed g_heap_top),
     * and before main so the compiler in libc.c sees a non-zero g_heap_top. */
    g_heap_top = _end;

    /* Write heap start address to the hardware heap-header slot at address 0.
     * volatile prevents the compiler from treating the null dereference as UB
     * and eliding/transforming the store.  Physical address 0 is valid RAM
     * on KlaussCPU (the hardware reserves the first 32 bytes as the heap
     * header; code starts at 0x0020). */
    *(volatile unsigned long long *)0 = (unsigned long long)(char *)_end;

    /* Checkpoint: BSS cleared, heap initialised, about to call main. */
    __builtin_klausscpu_leds(0xCCCC);

    main(0, (char **)0);

    /* Halt the CPU. */
    __builtin_trap();
}
