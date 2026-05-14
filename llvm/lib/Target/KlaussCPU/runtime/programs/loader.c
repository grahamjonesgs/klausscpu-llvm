/*
 * loader.c — KlaussCPU PIC program loader.
 *
 * Reads a flat PIC binary (.pic) from an SD card file, copies it to a
 * fixed region of RAM (PIC_LOAD_ADDR), and calls _start_loadable() at
 * that address.  The loaded program runs to completion and its return
 * value (main()'s exit code) is printed.
 *
 * PIC binary format:
 *   Produced by:  ld.lld -T klausscpu_pic.ld → elf; objcopy -O binary → .pic
 *   Content:      raw code + rodata + data + cleared BSS placeholder
 *   Entry point:  byte 0 of the file (_start_loadable)
 *
 * Memory layout:
 *   0x00000020 – 0x03FFFFFF : loader (≤ 64 MB)
 *   0x04000000 – 0x07FFFFFF : load area for PIC programs (64 MB)
 *
 * The loaded program may call printf/malloc/etc.; it shares the UART and
 * uses its own heap within its image (_end inside the loaded binary).
 *
 * SD card: FAT filesystem, file path passed as first argument to main().
 * Default filename if none given: "PROG.PIC" (8.3 uppercase on FAT).
 *
 * LEDs: bit0=SD init OK  bit1=file open OK  bit2=load OK  bit3=exec done
 */

#include <stdio.h>
#include <string.h>
#include "ff.h"
#include "../mmio.h"

/* Load address — must be 8-byte aligned, outside the loader's .text/.data. */
#define PIC_LOAD_ADDR   0x04000000UL
#define PIC_MAX_SIZE    (32UL * 1024 * 1024)   /* 32 MB limit */
#define DEFAULT_FILE    "PROG.PIC"

/* Type of the entry point: returns int (main()'s exit code). */
typedef int (*pic_entry_fn)(void);

/* ── SD card / FatFs state ─────────────────────────────────────────── */

static FATFS   g_fs;
static FIL     g_file;

/* ── Load and execute ─────────────────────────────────────────────── */

static int load_and_run(const char *path) {
    /* Mount the SD card. */
    FRESULT res = f_mount(&g_fs, "", 1);
    if (res != FR_OK) {
        printf("loader: f_mount failed (%d)\n", (int)res);
        return -1;
    }
    REG_LEDS |= 0x0001u;

    /* Open the PIC binary file. */
    res = f_open(&g_file, path, FA_READ);
    if (res != FR_OK) {
        printf("loader: cannot open '%s' (%d)\n", path, (int)res);
        f_unmount("");
        return -1;
    }
    REG_LEDS |= 0x0002u;

    FSIZE_t fsize = f_size(&g_file);
    printf("loader: '%s'  %lu bytes → 0x%08lX\n",
           path, (unsigned long)fsize, (unsigned long)PIC_LOAD_ADDR);

    if (fsize == 0 || fsize > PIC_MAX_SIZE) {
        printf("loader: file size out of range\n");
        f_close(&g_file);
        f_unmount("");
        return -1;
    }

    /* Read into load area. */
    uint8_t *dest = (uint8_t *)PIC_LOAD_ADDR;
    UINT    bytes_read = 0;
    UINT    to_read    = (UINT)fsize;

    res = f_read(&g_file, dest, to_read, &bytes_read);
    f_close(&g_file);
    f_unmount("");

    if (res != FR_OK || bytes_read != to_read) {
        printf("loader: read error (%d), got %u of %u bytes\n",
               (int)res, (unsigned)bytes_read, (unsigned)to_read);
        return -1;
    }
    REG_LEDS |= 0x0004u;

    printf("loader: loaded OK — calling entry at 0x%08lX\n",
           (unsigned long)PIC_LOAD_ADDR);

    /* Cast the load address to a function pointer and call it.
     * The loaded _start_loadable() clears the program's BSS, calls main(),
     * and returns main()'s exit code.  Stack and UART are inherited. */
    pic_entry_fn entry = (pic_entry_fn)PIC_LOAD_ADDR;
    int rc = entry();

    REG_LEDS |= 0x0008u;
    printf("\nloader: program returned %d\n", rc);
    return rc;
}

/* ── main ─────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== KlaussCPU PIC loader ===\n\n");
    REG_LEDS = 0x0000u;

    /* Allow a filename argument via a compile-time define, or use default. */
#ifndef PIC_FILENAME
#define PIC_FILENAME DEFAULT_FILE
#endif
    const char *path = PIC_FILENAME;
    printf("loading: %s\n", path);

    int rc = load_and_run(path);
    printf("done (rc=%d)\n", rc);
    return rc;
}
