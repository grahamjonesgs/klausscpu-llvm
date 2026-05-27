/* pic_loader.c — Load and run a PIC ELF from the SD card inside a Zephyr
 *                thread, mirroring the program's stdout to an SSH session.
 *
 * Ported from freertos/telnet/pic_loader.c.
 *
 * ELF format: ELF32 with PT_LOAD segments + SHT_RELA relocations,
 * produced by:  ld.lld --emit-relocs -T klausscpu_pic.ld
 *
 * Flat binary: any file not starting with the ELF magic is treated as a
 * position-independent flat binary; entry is at byte 0.
 *
 * Only one program runs at a time (g_pic_mutex serialises concurrent calls).
 */

#define WOLFSSL_USER_SETTINGS
#include "user_settings.h"

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include <stdio.h>
#include <string.h>

#include <wolfssh/ssh.h>

#include "pic_loader.h"

LOG_MODULE_REGISTER(pic_loader, LOG_LEVEL_INF);

/* ── PIC load address ────────────────────────────────────────────────────── */

#define PIC_LOAD_ADDR       0x04000000UL
#define PIC_MAX_SIZE        (48UL * 1024 * 1024)
#define PIC_STACK_SIZE      32768
#define PIC_THREAD_PRIO     7

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

typedef int (*pic_entry_fn)(void (*mirror_fn)(char));

/* ── Module state ─────────────────────────────────────────────────────────── */

static struct k_mutex g_pic_mutex;
static WOLFSSH *g_mirror_ssh;

/* ── Mirror function ─────────────────────────────────────────────────────── */

static void mirror_to_ssh(char c)
{
	WOLFSSH *ssh = g_mirror_ssh;

	if (!ssh) {
		return;
	}
	if (c == '\n') {
		uint8_t cr = '\r';
		wolfSSH_stream_send(ssh, &cr, 1);
	}
	wolfSSH_stream_send(ssh, (uint8_t *)&c, 1);
}

/* ── Output helper — SSH or printk ────────────────────────────────────────── */

static void loader_printf(WOLFSSH *ssh, const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0) {
		if (ssh) {
			wolfSSH_stream_send(ssh, (uint8_t *)buf, (word32)n);
		} else {
			printk("%s", buf);
		}
	}
}

/* ── Zephyr FS helpers ───────────────────────────────────────────────────── */

static struct fs_file_t s_file;

static int fread_at(uint32_t offset, void *buf, uint32_t len)
{
	if (fs_seek(&s_file, offset, FS_SEEK_SET) != 0) {
		return -1;
	}
	ssize_t got = fs_read(&s_file, buf, len);
	if (got < 0 || (uint32_t)got != len) {
		return -1;
	}
	return 0;
}

/* ── ELF loader ──────────────────────────────────────────────────────────── */

static int load_elf(WOLFSSH *ssh, u32 load_base, pic_entry_fn *entry_out)
{
	Elf32_Ehdr ehdr;

	if (fread_at(0, &ehdr, sizeof(ehdr)) != 0) {
		loader_printf(ssh, "ELF header read failed\r\n");
		return -1;
	}
	if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
	    ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
		loader_printf(ssh, "Bad ELF magic\r\n");
		return -1;
	}

	u32 link_base = 0xFFFFFFFFu;
	Elf32_Phdr phdr;

	for (int i = 0; i < ehdr.e_phnum; i++) {
		if (fread_at(ehdr.e_phoff + i * ehdr.e_phentsize, &phdr,
			     sizeof(phdr)) != 0) {
			continue;
		}
		if (phdr.p_type == PT_LOAD && phdr.p_vaddr < link_base) {
			link_base = phdr.p_vaddr;
		}
	}
	if (link_base == 0xFFFFFFFFu) {
		loader_printf(ssh, "No PT_LOAD segment\r\n");
		return -1;
	}

	s32 delta = (s32)(load_base - link_base);

	loader_printf(ssh, "link_base=0x%08lX  load=0x%08lX  delta=%ld\r\n",
		   (unsigned long)link_base, (unsigned long)load_base,
		   (long)delta);

	for (int i = 0; i < ehdr.e_phnum; i++) {
		if (fread_at(ehdr.e_phoff + i * ehdr.e_phentsize, &phdr,
			     sizeof(phdr)) != 0) {
			continue;
		}
		if (phdr.p_type != PT_LOAD) {
			continue;
		}

		u8 *dest = (u8 *)(u32)(phdr.p_vaddr + (u32)delta);

		if (phdr.p_filesz > 0) {
			if (fread_at(phdr.p_offset, dest, phdr.p_filesz) != 0) {
				loader_printf(ssh, "Segment read failed\r\n");
				return -1;
			}
		}
		if (phdr.p_memsz > phdr.p_filesz) {
			memset(dest + phdr.p_filesz, 0,
			       phdr.p_memsz - phdr.p_filesz);
		}
	}

	if (delta != 0 && ehdr.e_shoff != 0) {
		Elf32_Shdr shdr;

		for (int i = 0; i < ehdr.e_shnum; i++) {
			if (fread_at(ehdr.e_shoff + i * ehdr.e_shentsize, &shdr,
				     sizeof(shdr)) != 0) {
				continue;
			}
			if (shdr.sh_type != SHT_RELA || shdr.sh_entsize == 0) {
				continue;
			}

			u32 n = shdr.sh_size / shdr.sh_entsize;

			for (u32 j = 0; j < n; j++) {
				Elf32_Rela rela;

				if (fread_at(shdr.sh_offset +
						     j * shdr.sh_entsize,
					     &rela, sizeof(rela)) != 0) {
					break;
				}

				u32 type = ELF32_R_TYPE(rela.r_info);
				u8 *loc = (u8 *)(u32)(rela.r_offset +
						      (u32)delta);

				if (type == R_KLAUSSCPU_ABS32) {
					u32 old;
					memcpy(&old, loc, 4);
					u32 nw = old + (u32)delta;
					memcpy(loc, &nw, 4);
				} else if (type == R_KLAUSSCPU_ABS64) {
					unsigned long long old;
					memcpy(&old, loc, 8);
					unsigned long long nw =
						old +
						(unsigned long long)(
							long long)delta;
					memcpy(loc, &nw, 8);
				}
			}
		}
		loader_printf(ssh, "Relocations applied\r\n");
	}

	*entry_out = (pic_entry_fn)(u32)(ehdr.e_entry + (u32)delta);
	return 0;
}

/* ── Flat binary loader ──────────────────────────────────────────────────── */

static int load_flat(u32 load_base, size_t fsize, pic_entry_fn *entry_out)
{
	if (fs_seek(&s_file, 0, FS_SEEK_SET) != 0) {
		return -1;
	}
	ssize_t got = fs_read(&s_file, (void *)(uintptr_t)load_base, fsize);

	if (got < 0 || (size_t)got != fsize) {
		return -1;
	}
	*entry_out = (pic_entry_fn)load_base;
	return 0;
}

/* ── Top-level load (called with g_pic_mutex held) ───────────────────────── */

static int pic_load(const char *path, WOLFSSH *ssh, pic_entry_fn *entry_out)
{
	fs_file_t_init(&s_file);

	if (fs_open(&s_file, path, FS_O_READ) != 0) {
		loader_printf(ssh, "Cannot open '%s'\r\n", path);
		return -1;
	}

	struct fs_dirent stat;

	if (fs_stat(path, &stat) != 0) {
		loader_printf(ssh, "Cannot stat '%s'\r\n", path);
		fs_close(&s_file);
		return -1;
	}

	size_t fsize = stat.size;

	loader_printf(ssh, "'%s'  %lu bytes\r\n", path, (unsigned long)fsize);

	if (fsize == 0 || fsize > PIC_MAX_SIZE) {
		loader_printf(ssh, "File size out of range\r\n");
		fs_close(&s_file);
		return -2;
	}

	u8 magic[4];

	if (fread_at(0, magic, 4) != 0) {
		loader_printf(ssh, "Cannot read magic bytes\r\n");
		fs_close(&s_file);
		return -1;
	}
	int is_elf = (magic[0] == ELFMAG0 && magic[1] == ELFMAG1 &&
		      magic[2] == ELFMAG2 && magic[3] == ELFMAG3);

	int rc;

	if (is_elf) {
		loader_printf(ssh, "ELF format -> 0x%08lX\r\n",
			   (unsigned long)PIC_LOAD_ADDR);
		rc = load_elf(ssh, PIC_LOAD_ADDR, entry_out);
	} else {
		loader_printf(ssh, "Flat binary -> 0x%08lX\r\n",
			   (unsigned long)PIC_LOAD_ADDR);
		rc = load_flat(PIC_LOAD_ADDR, fsize, entry_out);
	}

	fs_close(&s_file);
	return rc;
}

/* ── Zephyr run thread ───────────────────────────────────────────────────── */

struct pic_run_args {
	pic_entry_fn entry;
	void (*mirror)(char);
	struct k_sem done;
	int result;
};

static K_THREAD_STACK_DEFINE(pic_stack, PIC_STACK_SIZE);
static struct k_thread pic_thread;

static void pic_run_entry(void *p1, void *p2, void *p3)
{
	struct pic_run_args *a = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	a->result = a->entry(a->mirror);
	k_sem_give(&a->done);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void pic_loader_init(void)
{
	k_mutex_init(&g_pic_mutex);
}

int pic_run_from_sd(const char *filename, WOLFSSH *ssh)
{
	k_mutex_lock(&g_pic_mutex, K_FOREVER);

	pic_entry_fn entry = NULL;
	int rc = pic_load(filename, ssh, &entry);

	if (rc < 0 || !entry) {
		k_mutex_unlock(&g_pic_mutex);
		return -1;
	}

	loader_printf(ssh, "Calling entry 0x%08lX ...\r\n",
		   (unsigned long)(u32)entry);

	struct pic_run_args args;

	args.entry = entry;
	args.mirror = ssh ? mirror_to_ssh : NULL;
	args.result = -1;
	k_sem_init(&args.done, 0, 1);

	g_mirror_ssh = ssh;

	k_thread_create(&pic_thread, pic_stack, PIC_STACK_SIZE,
			pic_run_entry, &args, NULL, NULL,
			PIC_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&pic_thread, "pic_run");

	k_sem_take(&args.done, K_FOREVER);

	g_mirror_ssh = NULL;
	k_mutex_unlock(&g_pic_mutex);

	return args.result;
}
