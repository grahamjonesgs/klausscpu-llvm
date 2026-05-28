/*
 * klausscpu_pic.ld — Linker script for PIC loadable programs.
 *
 * The image is linked from address 0.  The loader places it at any
 * 8-byte-aligned address in RAM and calls _start_loadable() at that
 * address.  All branches/calls use JMPREL/CALLREL (PC-relative PCREL32
 * fixups) so no runtime relocation patching is needed, provided the
 * program contains no absolute data-to-data pointers.
 *
 * Build:
 *   clang -target klausscpu-unknown-elf -fPIC -O1 \
 *         -isystem picolibc-install/include -nostdlib \
 *         -ffunction-sections -fdata-sections \
 *         -c -o myprog.o programs/myprog.c
 *   clang -target klausscpu-unknown-elf -fPIC -O1 \
 *         -isystem picolibc-install/include -nostdlib \
 *         -ffunction-sections -fdata-sections \
 *         -c -o crt0_loadable.o src/crt0_loadable.c
 *   ld.lld -T klausscpu_pic.ld --gc-sections -o myprog_pic.elf \
 *          crt0_loadable.o myprog.o syscalls.o stdio_handles.o setjmp.o \
 *          picolibc-install/lib/libc.a
 *   llvm-objcopy -O binary myprog_pic.elf myprog.pic
 *
 * The .pic binary is loaded by loader.c at PIC_LOAD_ADDR and executed.
 */

ENTRY(_start_loadable)

/*
 * The PIC binary is linked at PIC_LOAD_ADDR so that picolibc's internally
 * absolute CALL instructions resolve correctly at runtime.  Our application
 * code is compiled with -fPIC (LEAPC / JMPREL / CALLREL) and remains correct
 * at this base.  llvm-objcopy -O binary produces a flat image where byte 0 of
 * the file maps to address PIC_LOAD_ADDR — matching what loader.c expects.
 */
SECTIONS {
    . = 0x04000000;   /* must match PIC_LOAD_ADDR in loader.c */

    /* Entry code first — _start_loadable lands at byte 0 of the image. */
    .text : {
        *(.text.entry)
        *(.text .text.*)
        *(.gnu.linkonce.t.*)
    }

    . = ALIGN(8);
    .rodata : {
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r.*)
    }

    . = ALIGN(8);
    .data : {
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
    }

    . = ALIGN(8);
    __bss_start = .;
    .bss : {
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
    }
    . = ALIGN(8);
    __bss_end = .;

    /* _end marks the start of the heap within the loaded image. */
    . = ALIGN(16);
    _end = .;

    /* Heap bounds for picolibc's __fallback_sbrk / malloc.
     * The PIC load area is 0x04000000-0x07FFFFFF; the heap runs from just
     * past the loaded image up to 0x07E00000, staying clear of the Zephyr
     * boot/exception stack at 0x07F00000-0x07FFFFFF. */
    __heap_start = .;
    __heap_end   = 0x07E00000;

    /DISCARD/ : { *(.eh_frame) *(.comment) }
}
