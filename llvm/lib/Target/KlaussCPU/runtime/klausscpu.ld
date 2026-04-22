/*
 * KlaussCPU linker script — 128 MiB flat RAM
 *
 * Address space: 0x00000000 – 0x07FFFFFF (128 MiB)
 *
 * Layout:
 *   0x00000000  .text   (code + read-only data)
 *               .data   (initialised read-write data)
 *               .bss    (zero-filled data; cleared by crt0)
 *   _stack_top  top of RAM — hardware must initialise SP here before _start
 *               (or crt0 may set SP via __builtin_stack_restore if hardware
 *               provides a sane initial SP value through its prologue phase)
 */

ENTRY(_start)

MEMORY {
    RAM : ORIGIN = 0x00000000, LENGTH = 128M
}

SECTIONS {
    .text 0x00000000 : {
        KEEP(*(.text._start))   /* ensure _start lands at address 0 */
        *(.text*)
        *(.rodata*)
    } > RAM

    .data : ALIGN(8) {
        __data_start = .;
        *(.data*)
        __data_end = .;
    } > RAM

    .bss : ALIGN(8) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    } > RAM

    /* Symbols used by crt0 */
    _end        = .;
    _stack_top  = ORIGIN(RAM) + LENGTH(RAM);   /* = 0x08000000 */
}
