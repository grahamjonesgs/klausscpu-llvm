/*
 * KlaussCPU / Nexys A7 linker script for Zephyr.
 *
 * RAM: 128 MiB at 0x00000000.
 *   0x00000000–0x0000001F : hardware heap header (8 bytes used by hw)
 *   0x00000020            : code start (_start)
 *   0x04000000            : .data/.bss base (60 MiB data region)
 *   0x07F00000–0x07FFFFFF : stack (1 MiB)
 *
 * MMIO: 0xF0000000 and above.
 */

#include <zephyr/linker/sections.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/linker/linker-tool.h>

/*
 * RAMABLE_REGION origin chosen to keep the gap between rodata end and data
 * start within klausscc's MAX_SEGMENT_GAP (1 MB).  Otherwise klausscc's
 * parse_elf_to_flat silently drops the .data/.bss segment and timeout_list
 * (plus every other initialised global) never reaches RAM.
 *
 * Layout:
 *   0x00000000–0x0000001F : hardware heap header (8 bytes used)
 *   0x00000020–0x000FFFFF : ROM (text + rodata + initlevel etc., up to 1 MB)
 *   0x00100000–0x07EFFFFF : RAM (data + bss + .noinit + thread stacks)
 *   0x07F00000–0x07FFFFFF : boot/exception stack (1 MB at top of 128 MB RAM)
 */
MEMORY
{
    ROM (rx)  : ORIGIN = 0x00000020, LENGTH = 1M
    RAMABLE_REGION (rwx) : ORIGIN = 0x00100000, LENGTH = 126M
    STACK (rw): ORIGIN = 0x07F00000, LENGTH = 1M
}

#define ROMABLE_REGION  ROM
#define LINKER_DT_REGIONS_ROM  ROM
#define DT_CHOSEN_Z_SRAM    RAMABLE_REGION

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
