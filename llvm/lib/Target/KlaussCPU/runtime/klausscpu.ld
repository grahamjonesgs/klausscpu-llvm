/*
 * KlaussCPU linker script — 128 MiB flat RAM
 *
 * Address space: 0x00000000 – 0x07FFFFFF (128 MiB)
 *
 * The first 32 bytes (0x00000000–0x0000001F) are the hardware heap header.
 * Code starts at 0x00000020. The flat binary is loaded by the hardware at
 * address 0x00000020, so byte 0 of the .bin file = first instruction.
 *
 * Layout:
 *   0x00000000–0x0000001F  heap header (hardware reserved, not in binary)
 *   0x00000020  .text   (_start, then remaining code + read-only data)
 *               .data   (initialised read-write data)
 *               .bss    (zero-filled data; cleared by crt0)
 *   _stack_top  top of RAM — hardware must initialise SP here before _start
 */

ENTRY(_start)

MEMORY {
    RAM : ORIGIN = 0x00000000, LENGTH = 128M
}

SECTIONS {
    .text 0x00000020 : {
        KEEP(*(.text._start))   /* _start lands at 0x00000020 */
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

    /* Heap bounds used by picolibc's __fallback_sbrk / malloc */
    __heap_start = .;
    __heap_end   = ORIGIN(RAM) + LENGTH(RAM) - 0x10000;  /* leave 64 KiB for stack */

    /* Symbols used by crt0 and picolibc */
    _end        = .;
    _stack_top  = ORIGIN(RAM) + LENGTH(RAM);   /* = 0x08000000 */
}
