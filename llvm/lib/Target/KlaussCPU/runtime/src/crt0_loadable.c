/*
 * crt0_loadable.c — Minimal startup for PIC loadable programs.
 *
 * Entry point: _start_loadable (placed in .text.entry so it lands at the
 * start of the loaded binary).  The loader calls this as a C function
 * pointer; it clears BSS, calls main(), and returns main()'s exit code
 * to the loader.  No HALT — control returns to the host program.
 *
 * Stack: inherited from the loader (same physical stack, grows down).
 * stdio: inherited from the loader (UART already initialized).
 * Heap:  _sbrk uses _end which is within the loaded image.  malloc() calls
 *        from the loadable program are independent of the loader's heap.
 */

extern int  main(void);
extern void __stdio_init(void);   /* from stdio_handles.c — wires UART to stdout/stderr */

/* __attribute__((section)) ensures this function's code is emitted first
 * in the .text.entry section, so it appears at the binary's start address.
 * The PIC linker script places .text.entry before .text. */
/* Console hooks installed when non-NULL.  A loader that wants to drive the
 * program over SSH/telnet passes an output mirror and an input source; bare
 * callers pass NULL for both (console I/O goes to the physical UART).
 *
 * ABI note: the loader calls this with two arguments.  Older single-argument
 * binaries remain compatible (the extra register is simply ignored), but a
 * binary built with this ABI must be run by a loader that supplies both. */
extern void (*g_console_mirror_fn)(char c);
extern int  (*g_console_input_fn)(void);

__attribute__((section(".text.entry"), noinline))
int _start_loadable(void (*mirror_fn)(char), int (*input_fn)(void)) {
    /* Clear BSS within the loaded image. */
    extern char __bss_start[], __bss_end[];
    for (char *p = __bss_start; p < __bss_end; ++p)
        *p = 0;

    /* Initialise stdio — must happen after BSS clear (FILE objects are in BSS). */
    __stdio_init();

    /* Install the caller's console hooks (NULL = physical UART). */
    g_console_mirror_fn = mirror_fn;
    g_console_input_fn  = input_fn;

    return main();
}
