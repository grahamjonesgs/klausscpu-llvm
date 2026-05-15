/*
 * pic_loader.c — Load and run a PIC ELF from the SD card inside a FreeRTOS
 *                task, mirroring the program's stdout to a telnet socket.
 *
 * ELF format: ELF32 with PT_LOAD segments + SHT_RELA relocations,
 * produced by:  ld.lld --emit-relocs -T klausscpu_pic.ld
 *
 * Flat binary: any file not starting with the ELF magic is treated as a
 * position-independent flat binary; entry is at byte 0.
 *
 * Only one program runs at a time (g_pic_mutex serialises concurrent calls).
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "lwip/sockets.h"

#include "ff.h"
#include "pic_loader.h"

/* ── PIC load address ────────────────────────────────────────────────────── */

#define PIC_LOAD_ADDR       0x04000000UL
#define PIC_MAX_SIZE        (48UL * 1024 * 1024)
#define PIC_TASK_STACK_WORDS 4096   /* 32 KB — must hold the PIC program's call stack */
#define PIC_TASK_PRIO        2

/* ── ELF32 minimal types ─────────────────────────────────────────────────── */

#define ELFMAG0  0x7fu
#define ELFMAG1  'E'
#define ELFMAG2  'L'
#define ELFMAG3  'F'
#define PT_LOAD  1u
#define SHT_RELA 4u

#define R_KLAUSSCPU_NONE    0u
#define R_KLAUSSCPU_ABS32   1u
#define R_KLAUSSCPU_ABS64   2u
#define R_KLAUSSCPU_PCREL32 3u

#define ELF32_R_TYPE(info) ((info) & 0xFFu)

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef int            s32;

typedef struct {
    u8  e_ident[16];
    u16 e_type, e_machine;
    u32 e_version;
    u32 e_entry;
    u32 e_phoff;
    u32 e_shoff;
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
    u32 r_offset;
    u32 r_info;
    s32 r_addend;
} Elf32_Rela;

/* Entry function signature matches crt0_loadable _start_loadable. */
typedef int (*pic_entry_fn)(void (*mirror_fn)(char));

/* ── Module state ─────────────────────────────────────────────────────────── */

static SemaphoreHandle_t g_pic_mutex;
static int               g_mirror_sock = -1;

/* FatFs objects are large; keep them static (protected by g_pic_mutex). */
static FATFS  s_fs;
static FIL    s_file;

/* ── Mirror function ─────────────────────────────────────────────────────── */

/* Called from the PIC program's _uart_put via the function-pointer argument
 * passed to entry().  Converts \n → \r\n for telnet and sends to the socket. */
static void mirror_to_telnet(char c) {
    int sock = g_mirror_sock;
    if (sock < 0) return;
    if (c == '\n') {
        char cr = '\r';
        lwip_send(sock, &cr, 1, 0);
    }
    lwip_send(sock, &c, 1, 0);
}

/* ── Socket helper ───────────────────────────────────────────────────────── */

static void sock_printf(int sock, const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    if (n > 0)
        lwip_send(sock, buf, n, 0);
}

/* ── FatFs helper ─────────────────────────────────────────────────────────── */

static int fread_at(u32 offset, void *buf, u32 len) {
    UINT got;
    if (f_lseek(&s_file, offset) != FR_OK) return -1;
    if (f_read(&s_file, buf, len, &got) != FR_OK || got != len) return -1;
    return 0;
}

/* ── ELF loader ──────────────────────────────────────────────────────────── */

static int load_elf(int sock, u32 load_base, pic_entry_fn *entry_out) {
    Elf32_Ehdr ehdr;
    if (fread_at(0, &ehdr, sizeof(ehdr)) != 0) {
        sock_printf(sock, "ELF header read failed\r\n");
        return -1;
    }
    if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
        ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
        sock_printf(sock, "Bad ELF magic\r\n");
        return -1;
    }

    u32 link_base = 0xFFFFFFFFu;
    Elf32_Phdr phdr;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (fread_at(ehdr.e_phoff + i * ehdr.e_phentsize, &phdr,
                     sizeof(phdr)) != 0) continue;
        if (phdr.p_type == PT_LOAD && phdr.p_vaddr < link_base)
            link_base = phdr.p_vaddr;
    }
    if (link_base == 0xFFFFFFFFu) {
        sock_printf(sock, "No PT_LOAD segment\r\n");
        return -1;
    }

    s32 delta = (s32)(load_base - link_base);
    sock_printf(sock, "link_base=0x%08lX  load=0x%08lX  delta=%ld\r\n",
                (unsigned long)link_base, (unsigned long)load_base, (long)delta);

    /* Load all PT_LOAD segments. */
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (fread_at(ehdr.e_phoff + i * ehdr.e_phentsize, &phdr,
                     sizeof(phdr)) != 0) continue;
        if (phdr.p_type != PT_LOAD) continue;

        u8 *dest = (u8 *)(u32)(phdr.p_vaddr + (u32)delta);
        if (phdr.p_filesz > 0) {
            if (fread_at(phdr.p_offset, dest, phdr.p_filesz) != 0) {
                sock_printf(sock, "Segment read failed\r\n");
                return -1;
            }
        }
        if (phdr.p_memsz > phdr.p_filesz)
            memset(dest + phdr.p_filesz, 0, phdr.p_memsz - phdr.p_filesz);
    }

    /* Apply ABS32/ABS64 relocations (skip PCREL32 — already correct). */
    if (delta != 0 && ehdr.e_shoff != 0) {
        Elf32_Shdr shdr;
        for (int i = 0; i < ehdr.e_shnum; i++) {
            if (fread_at(ehdr.e_shoff + i * ehdr.e_shentsize, &shdr,
                         sizeof(shdr)) != 0) continue;
            if (shdr.sh_type != SHT_RELA || shdr.sh_entsize == 0) continue;

            u32 n = shdr.sh_size / shdr.sh_entsize;
            for (u32 j = 0; j < n; j++) {
                Elf32_Rela rela;
                if (fread_at(shdr.sh_offset + j * shdr.sh_entsize, &rela,
                             sizeof(rela)) != 0) break;

                u32 type = ELF32_R_TYPE(rela.r_info);
                u8 *loc  = (u8 *)(u32)(rela.r_offset + (u32)delta);

                if (type == R_KLAUSSCPU_ABS32) {
                    u32 old; memcpy(&old, loc, 4);
                    u32 nw = old + (u32)delta;
                    memcpy(loc, &nw, 4);
                } else if (type == R_KLAUSSCPU_ABS64) {
                    unsigned long long old; memcpy(&old, loc, 8);
                    unsigned long long nw = old + (unsigned long long)(long long)delta;
                    memcpy(loc, &nw, 8);
                }
            }
        }
        sock_printf(sock, "Relocations applied\r\n");
    }

    *entry_out = (pic_entry_fn)(u32)(ehdr.e_entry + (u32)delta);
    return 0;
}

/* ── Flat binary loader ──────────────────────────────────────────────────── */

static int load_flat(int sock, u32 load_base, FSIZE_t fsize,
                     pic_entry_fn *entry_out) {
    (void)sock;
    UINT got;
    if (f_lseek(&s_file, 0) != FR_OK) return -1;
    FRESULT res = f_read(&s_file, (u8 *)load_base, (UINT)fsize, &got);
    if (res != FR_OK || got != (UINT)fsize) return -1;
    *entry_out = (pic_entry_fn)load_base;
    return 0;
}

/* ── Top-level load (called with g_pic_mutex held) ───────────────────────── */

static int pic_load(const char *filename, int sock, pic_entry_fn *entry_out) {
    FRESULT res = f_mount(&s_fs, "", 1);
    if (res != FR_OK) {
        sock_printf(sock, "SD mount failed (%d)\r\n", (int)res);
        return -1;
    }

    res = f_open(&s_file, filename, FA_READ);
    if (res != FR_OK) {
        sock_printf(sock, "Cannot open '%s' (%d)\r\n", filename, (int)res);
        f_unmount("");
        return -1;
    }

    FSIZE_t fsize = f_size(&s_file);
    sock_printf(sock, "'%s'  %lu bytes\r\n", filename, (unsigned long)fsize);

    if (fsize == 0 || fsize > PIC_MAX_SIZE) {
        sock_printf(sock, "File size out of range\r\n");
        f_close(&s_file);
        f_unmount("");
        return -2;
    }

    u8 magic[4];
    UINT got;
    f_read(&s_file, magic, 4, &got);
    int is_elf = (got == 4 && magic[0] == ELFMAG0 && magic[1] == ELFMAG1 &&
                               magic[2] == ELFMAG2 && magic[3] == ELFMAG3);

    int rc;
    if (is_elf) {
        sock_printf(sock, "ELF format → 0x%08lX\r\n", (unsigned long)PIC_LOAD_ADDR);
        rc = load_elf(sock, PIC_LOAD_ADDR, entry_out);
    } else {
        sock_printf(sock, "Flat binary → 0x%08lX\r\n", (unsigned long)PIC_LOAD_ADDR);
        rc = load_flat(sock, PIC_LOAD_ADDR, fsize, entry_out);
    }

    f_close(&s_file);
    f_unmount("");
    return rc;
}

/* ── FreeRTOS run task ───────────────────────────────────────────────────── */

typedef struct {
    pic_entry_fn     entry;
    SemaphoreHandle_t done;
    int               result;
} pic_run_args_t;

static void pic_run_task(void *arg) {
    pic_run_args_t *a = (pic_run_args_t *)arg;
    a->result = a->entry(mirror_to_telnet);
    xSemaphoreGive(a->done);
    vTaskDelete(NULL);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void pic_loader_init(void) {
    g_pic_mutex = xSemaphoreCreateMutex();
    configASSERT(g_pic_mutex);
}

int pic_run_from_sd(const char *filename, int sock) {
    xSemaphoreTake(g_pic_mutex, portMAX_DELAY);

    pic_entry_fn entry = NULL;
    int rc = pic_load(filename, sock, &entry);
    if (rc < 0 || !entry) {
        xSemaphoreGive(g_pic_mutex);
        return -1;
    }

    sock_printf(sock, "Calling entry 0x%08lX ...\r\n",
                (unsigned long)(u32)entry);

    pic_run_args_t args;
    args.entry  = entry;
    args.result = -1;
    args.done   = xSemaphoreCreateBinary();
    if (!args.done) {
        xSemaphoreGive(g_pic_mutex);
        return -3;
    }

    g_mirror_sock = sock;

    BaseType_t ok = xTaskCreate(pic_run_task, "PICRUN",
                                PIC_TASK_STACK_WORDS, &args,
                                PIC_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        vSemaphoreDelete(args.done);
        g_mirror_sock = -1;
        xSemaphoreGive(g_pic_mutex);
        return -3;
    }

    /* Block here while the PIC program runs; args stays valid on our stack. */
    xSemaphoreTake(args.done, portMAX_DELAY);
    vSemaphoreDelete(args.done);

    g_mirror_sock = -1;
    xSemaphoreGive(g_pic_mutex);
    return args.result;
}
