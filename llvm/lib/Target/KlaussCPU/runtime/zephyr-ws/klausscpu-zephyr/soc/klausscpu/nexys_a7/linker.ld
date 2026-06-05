/*
 * KlaussCPU / Nexys A7 linker script for Zephyr.
 *
 * RAM: 128 MiB at 0x00000000 (unified — no separate ROM/flash).
 *   0x00000000–0x0000001F : hardware heap header (8 bytes used by hw)
 *   0x00000020            : code start (_start); text/rodata/data/bss contiguous
 *   _end                  : heap, up to 0x07EFFFFF
 *   0x07F00000–0x07FFFFFF : stack (1 MiB)
 *
 * MMIO: 0xF0000000 and above.
 */

#include <zephyr/linker/sections.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/linker/linker-tool.h>

/*
 * KlaussCPU has unified RAM (no separate ROM/flash) and is built CONFIG_XIP=n,
 * so text, rodata, .data and .bss all live in ONE region laid out CONTIGUOUSLY:
 * .data immediately follows rodata (GROUP_DATA_LINK_IN with XIP=n gives
 * LMA==VMA, so the boot data-copy is a no-op).  The flat ELF loader therefore
 * sees a single contiguous image with NO inter-segment gap — there is no
 * klausscc MAX_SEGMENT_GAP concern, and code+rodata can grow to any size up to
 * the RAM region (no fixed ROM cap).
 *
 * NB: this replaced an earlier split ROM(0x20)+RAMABLE(fixed origin) layout.
 * With XIP=n that put .data at the fixed RAMABLE origin, leaving a
 * (origin - rodata_end) hole the loader had to bridge — and a large hole (e.g.
 * a 4 MB ROM region → ~3 MB hole) made the loader drop the .data/.bss segment
 * entirely → garbage globals + crash.  A single region has no hole.
 *
 * Layout (contiguous from 0x20):
 *   0x00000000–0x0000001F : hardware heap header (8 bytes used)
 *   0x00000020–_end       : text + rodata + .data + .bss + kernel objects
 *   _end     –0x07EFFFFF  : heap (to top of RAM region)
 *   0x07F00000–0x07FFFFFF : boot/exception stack (1 MB at top of 128 MB RAM)
 */
MEMORY
{
    RAM   (rwx): ORIGIN = 0x00000020, LENGTH = 0x07EFFFE0  /* 0x20 .. 0x07F00000 */
    STACK (rw) : ORIGIN = 0x07F00000, LENGTH = 1M
}

/* One unified region: point every ROM/RAM region name at it so text/rodata and
 * data/bss are placed contiguously (GROUP_*_LINK_IN just emit "> <region>"). */
#define ROM                    RAM
#define ROMABLE_REGION         RAM
#define RAMABLE_REGION         RAM
#define LINKER_DT_REGIONS_ROM  RAM
#define DT_CHOSEN_Z_SRAM       RAM

PROVIDE (_image_ram_start = ORIGIN(RAMABLE_REGION));

ENTRY(CONFIG_KERNEL_ENTRY)

SECTIONS
{
    GROUP_START(ROM)

    _image_rom_start = .;

    SECTION_PROLOGUE(_TEXT_SECTION_NAME,,)
    {
        __text_region_start = .;
        *(.text._start)
        *(.text*)
        *(.gnu.linkonce.t.*)
    } GROUP_LINK_IN(ROM)

    __text_region_end = .;

#include <zephyr/linker/common-rom/common-rom-kernel-devices.ld>
#include <zephyr/linker/common-rom/common-rom-logging.ld>
#include <zephyr/linker/common-rom/common-rom-net.ld>
#include <zephyr/linker/common-rom/common-rom-misc.ld>

    SECTION_PROLOGUE(_RODATA_SECTION_NAME,,)
    {
        *(.rodata*)
        *(.gnu.linkonce.r.*)
        *(.srodata*)
    } GROUP_LINK_IN(ROM)

    _image_rom_end = .;

    GROUP_END(ROM)

    GROUP_START(RAMABLE_REGION)

    __data_region_start = .;

    SECTION_DATA_PROLOGUE(_DATA_SECTION_NAME,,)
    {
        __data_start = .;
        *(.data*)
        *(.gnu.linkonce.d.*)
        __data_end = .;
    } GROUP_DATA_LINK_IN(RAMABLE_REGION, ROM)
    __data_load_start = LOADADDR(datas);

    SECTION_DATA_PROLOGUE(_BSS_SECTION_NAME,(NOLOAD),)
    {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        *(.gnu.linkonce.b.*)
        __bss_end = .;
    } GROUP_DATA_LINK_IN(RAMABLE_REGION, RAMABLE_REGION)

    __data_region_end = .;

    SECTION_DATA_PROLOGUE(.noinit,(NOLOAD),)
    {
        *(.noinit*)
    } GROUP_DATA_LINK_IN(RAMABLE_REGION, RAMABLE_REGION)

#include <zephyr/linker/common-ram.ld>

    /* LMA of the second initialized-data group (device_states, k_heap_area, …).
     * __start in swap.S copies this segment from ROM to RAM after copying .datas. */
    __device_states_load_start = LOADADDR(device_states);
    __kernel_initialized_end = .;

    _end = .;

    __heap_start = _end;
    __heap_end   = ORIGIN(STACK);

    GROUP_END(RAMABLE_REGION)

    _image_ram_end = ORIGIN(STACK) + LENGTH(STACK);
    _stack_end     = ORIGIN(STACK);
    _stack_start   = _image_ram_end;
}
