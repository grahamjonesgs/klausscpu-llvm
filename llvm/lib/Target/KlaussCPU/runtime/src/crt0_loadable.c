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
/* Mirror function installed in g_console_mirror_fn when non-NULL.
 * Callers that want stdout mirrored (e.g. telnet loader) pass a function
 * pointer; bare callers pass NULL. */
extern void (*g_console_mirror_fn)(char c);

__attribute__((section(".text.entry"), noinline))
int _start_loadable(void (*mirror_fn)(char)) {
    /* Clear BSS within the loaded image. */
    extern char __bss_start[], __bss_end[];
    for (char *p = __bss_start; p < __bss_end; ++p)
        *p = 0;

    /* Initialise stdio — must happen after BSS clear (FILE objects are in BSS). */
    __stdio_init();

    /* Install the caller's mirror hook (NULL = UART only). */
    g_console_mirror_fn = mirror_fn;

    return main();
}
