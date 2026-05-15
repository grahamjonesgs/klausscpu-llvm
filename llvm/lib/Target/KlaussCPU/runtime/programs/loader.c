/*
 * loader.c — KlaussCPU PIC program loader.
 *
 * Loads a PIC program from the SD card and runs it.  Two formats supported,
 * detected automatically by the first 4 bytes of the file:
 *
 *   ELF format  (magic 0x7f 'E' 'L' 'F'):
 *     Produced by:  ld.lld --emit-relocs -T klausscpu_pic.ld → _pic.elf
 *     Optionally stripped:  llvm-strip --strip-debug → .sd.elf
 *     Content:      ELF32 with PT_LOAD segments + SHT_RELA relocation sections.
 *     Runtime fixup: if loaded at a different address than the link base,
 *                    ABS32/ABS64 data pointers are patched by delta.
 *                    All code uses CALLREL/JMPREL/LEAPC (already correct).
 *
 *   Flat binary (any other magic):
 *     Produced by:  llvm-objcopy -O binary _pic.elf → .pic
 *     Entry point:  byte 0 (_start_loadable).
 *     Backward compat only; no runtime relocation support.
 *
 * Memory layout:
 *   0x00000020 – 0x03FFFFFF : loader + its libraries (≤ 64 MB)
 *   0x04000000 – 0x07FFFFFF : PIC program load area (64 MB)
 *
 * SD card file: default PROG.ELF (ELF) or PROG.PIC (flat binary).
 * LEDs: bit0=SD init  bit1=file open  bit2=load OK  bit3=exec done
 */

#include <stdio.h>
#include <string.h>
#include "ff.h"
#include "../mmio.h"

/* ── Load address ──────────────────────────────────────────────────────── */
#define PIC_LOAD_ADDR   0x04000000UL
#define PIC_MAX_SIZE    (48UL * 1024 * 1024)   /* 48 MB limit */
#ifndef PIC_FILENAME
#  define PIC_FILENAME  "PROG.ELF"
#endif

typedef int (*pic_entry_fn)(void);

/* ── Minimal ELF32 types (no <elf.h> in freestanding environment) ──────── */

#define ELFMAG0  0x7fu
#define ELFMAG1  'E'
#define ELFMAG2  'L'
#define ELFMAG3  'F'
#define PT_LOAD  1u
#define SHT_RELA 4u

/* KlaussCPU relocation types */
#define R_KLAUSSCPU_NONE    0u
#define R_KLAUSSCPU_ABS32   1u
#define R_KLAUSSCPU_ABS64   2u
#define R_KLAUSSCPU_PCREL32 3u

#define ELF32_R_TYPE(info)  ((info) & 0xFFu)

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef int            s32;

typedef struct {
    u8  e_ident[16];
    u16 e_type, e_machine;
    u32 e_version;
    u32 e_entry;          /* VA of entry point */
    u32 e_phoff;          /* program header table offset */
    u32 e_shoff;          /* section header table offset */
    u32 e_flags;
    u16 e_ehsize, e_phentsize, e_phnum;
    u16 e_shentsize, e_shnum, e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    u32 p_type, p_offset, p_vaddr, p_paddr;
    u32 p_filesz, p_memsz, p_flags, p_align;
} Elf32_Phdr;

typedef struct {
    u32 sh_name, sh_type, sh_flags;
    u32 sh_addr, sh_offset, sh_size;
    u32 sh_link, sh_info, sh_addralign, sh_entsize;
} Elf32_Shdr;

typedef struct {
    u32 r_offset;   /* VA of the field to patch */
    u32 r_info;     /* symbol + type (low byte = type) */
    s32 r_addend;
} Elf32_Rela;

/* ── FatFs helpers ─────────────────────────────────────────────────────── */
static FATFS  g_fs;
static FIL    g_file;

static int fread_at(u32 offset, void *buf, u32 len) {
    UINT got;
    if (f_lseek(&g_file, offset) != FR_OK) return -1;
    if (f_read(&g_file, buf, len, &got) != FR_OK || got != len) return -1;
    return 0;
}

/* ── ELF loader ────────────────────────────────────────────────────────── */

static int load_elf(u32 load_base, pic_entry_fn *entry_out) {
    Elf32_Ehdr ehdr;
    if (fread_at(0, &ehdr, sizeof(ehdr)) != 0) {
        printf("loader: ELF header read failed\n");
        return -1;
    }
    if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
        ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
        printf("loader: bad ELF magic\n");
        return -1;
    }

    /* Find link base = lowest PT_LOAD VMA. */
    u32 link_base = 0xFFFFFFFFu;
    Elf32_Phdr phdr;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (fread_at(ehdr.e_phoff + i * ehdr.e_phentsize, &phdr,
                     sizeof(phdr)) != 0) continue;
        if (phdr.p_type == PT_LOAD && phdr.p_vaddr < link_base)
            link_base = phdr.p_vaddr;
    }
    if (link_base == 0xFFFFFFFFu) { printf("loader: no PT_LOAD\n"); return -1; }

    s32 delta = (s32)(load_base - link_base);
    printf("loader: link_base=0x%08lX  load_base=0x%08lX  delta=%ld\n",
           (unsigned long)link_base, (unsigned long)load_base, (long)delta);

    /* Load all PT_LOAD segments. */
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (fread_at(ehdr.e_phoff + i * ehdr.e_phentsize, &phdr,
                     sizeof(phdr)) != 0) continue;
        if (phdr.p_type != PT_LOAD) continue;

        u8 *dest = (u8 *)(u32)(phdr.p_vaddr + (u32)delta);
        if (phdr.p_filesz > 0) {
            if (fread_at(phdr.p_offset, dest, phdr.p_filesz) != 0) {
                printf("loader: segment read failed\n");
                return -1;
            }
        }
        /* Zero BSS (memsz > filesz). */
        if (phdr.p_memsz > phdr.p_filesz)
            memset(dest + phdr.p_filesz, 0, phdr.p_memsz - phdr.p_filesz);
    }

    /* Apply ABS32/ABS64 relocations (only needed when delta != 0).
     * PCREL32 entries are skipped — they are already correct for any base.
     * Requires the ELF to have been linked with lld --emit-relocs. */
    if (delta != 0 && ehdr.e_shoff != 0) {
        Elf32_Shdr shdr;
        for (int i = 0; i < ehdr.e_shnum; i++) {
            if (fread_at(ehdr.e_shoff + i * ehdr.e_shentsize, &shdr,
                         sizeof(shdr)) != 0) continue;
            if (shdr.sh_type != SHT_RELA) continue;
            if (shdr.sh_entsize == 0) continue;

            u32 n = shdr.sh_size / shdr.sh_entsize;
            for (u32 j = 0; j < n; j++) {
                Elf32_Rela rela;
                if (fread_at(shdr.sh_offset + j * shdr.sh_entsize, &rela,
                             sizeof(rela)) != 0) break;

                u32 type = ELF32_R_TYPE(rela.r_info);
                u8 *loc  = (u8 *)(u32)(rela.r_offset + (u32)delta);

                if (type == R_KLAUSSCPU_ABS32) {
                    u32 old;
                    memcpy(&old, loc, 4);
                    u32 nw = old + (u32)delta;
                    memcpy(loc, &nw, 4);
                } else if (type == R_KLAUSSCPU_ABS64) {
                    unsigned long long old;
                    memcpy(&old, loc, 8);
                    unsigned long long nw = old + (unsigned long long)(long long)delta;
                    memcpy(loc, &nw, 8);
                }
                /* PCREL32 / NONE: no fixup needed */
            }
        }
        printf("loader: relocations applied\n");
    }

    *entry_out = (pic_entry_fn)(u32)(ehdr.e_entry + (u32)delta);
    return 0;
}

/* ── Flat binary loader (legacy .pic format) ────────────────────────────── */

static int load_flat(u32 load_base, FSIZE_t fsize, pic_entry_fn *entry_out) {
    u8   *dest = (u8 *)load_base;
    UINT  got;
    if (f_lseek(&g_file, 0) != FR_OK) return -1;
    FRESULT res = f_read(&g_file, dest, (UINT)fsize, &got);
    if (res != FR_OK || got != (UINT)fsize) {
        printf("loader: flat read error (%d)\n", (int)res);
        return -1;
    }
    *entry_out = (pic_entry_fn)load_base;
    return 0;
}

/* ── Top-level load + run ───────────────────────────────────────────────── */

static int load_and_run(const char *path) {
    FRESULT res = f_mount(&g_fs, "", 1);
    if (res != FR_OK) {
        printf("loader: f_mount failed (%d)\n", (int)res);
        return -1;
    }
    REG_LEDS |= 0x0001u;

    res = f_open(&g_file, path, FA_READ);
    if (res != FR_OK) {
        printf("loader: cannot open '%s' (%d)\n", path, (int)res);
        f_unmount("");
        return -1;
    }
    REG_LEDS |= 0x0002u;

    FSIZE_t fsize = f_size(&g_file);
    printf("loader: '%s'  %lu bytes\n", path, (unsigned long)fsize);

    if (fsize == 0 || fsize > PIC_MAX_SIZE) {
        printf("loader: file size out of range\n");
        f_close(&g_file); f_unmount(""); return -1;
    }

    /* Detect format by ELF magic. */
    u8 magic[4];
    UINT got;
    f_read(&g_file, magic, 4, &got);
    int is_elf = (got == 4 && magic[0] == ELFMAG0 && magic[1] == ELFMAG1 &&
                               magic[2] == ELFMAG2 && magic[3] == ELFMAG3);

    pic_entry_fn entry = NULL;
    int rc;
    if (is_elf) {
        printf("loader: ELF format detected → 0x%08lX\n",
               (unsigned long)PIC_LOAD_ADDR);
        rc = load_elf(PIC_LOAD_ADDR, &entry);
    } else {
        printf("loader: flat binary → 0x%08lX\n", (unsigned long)PIC_LOAD_ADDR);
        rc = load_flat(PIC_LOAD_ADDR, fsize, &entry);
    }

    f_close(&g_file);
    f_unmount("");

    if (rc != 0 || !entry) {
        printf("loader: load failed\n");
        return -1;
    }
    REG_LEDS |= 0x0004u;
    printf("loader: calling entry 0x%08lX\n", (unsigned long)(u32)entry);

    rc = entry();

    REG_LEDS |= 0x0008u;
    printf("\nloader: program returned %d\n", rc);
    return rc;
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== KlaussCPU PIC loader ===\n\n");
    REG_LEDS = 0x0000u;

    printf("loading: %s\n", PIC_FILENAME);
    int rc = load_and_run(PIC_FILENAME);
    printf("done (rc=%d)\n", rc);
    return rc;
}
