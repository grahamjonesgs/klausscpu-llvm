/* llext_loader.c — Load and run an LLEXT extension from the SD card inside a
 *                  Zephyr thread, redirecting the extension's stdio to an SSH
 *                  session.
 *
 * Replaces the custom PIC loader (pic_loader.c).  Extensions are ELFCLASS32
 * ET_REL objects produced by the KlaussCPU clang with `-c`; they call the
 * kernel's exported libc (see llext_exports.c) rather than bundling their own.
 *
 * The whole file is read into a RAM buffer and handed to the llext buf loader,
 * which copies/relocates the sections into the llext heap.  We then resolve the
 * extension's `main`, install the console redirect hooks, and run main() on a
 * dedicated thread (so the connection thread can park without touching wolfSSH
 * concurrently).
 *
 * Only one extension runs at a time (g_ext_mutex serialises concurrent calls).
 */

#define WOLFSSL_USER_SETTINGS
#include "user_settings.h"

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/llext/llext.h>
#include <zephyr/llext/buf_loader.h>
#include <zephyr/logging/log.h>

#include <stdio.h>
#include <string.h>

#include <wolfssh/ssh.h>

#include "llext_loader.h"

LOG_MODULE_REGISTER(llext_loader, LOG_LEVEL_INF);

#define EXT_MAX_SIZE    (8UL * 1024 * 1024)
#define EXT_STACK_SIZE  32768
#define EXT_THREAD_PRIO 7

/* Console redirect hooks (defined in arch/klausscpu/core/irq.c and
 * ssh/llext_exports.c).  NULL = physical UART / no input. */
extern int (*klausscpu_console_out_hook)(int c);
extern int (*klausscpu_console_in_hook)(void);

typedef int (*ext_entry_fn)(int argc, char **argv);

static struct k_mutex g_ext_mutex;
static WOLFSSH *g_session;

/* ── Console redirect callbacks (run on the ext thread) ───────────────────── */

static int mirror_out(int c)
{
	WOLFSSH *ssh = g_session;

	if (ssh == NULL) {
		return c;
	}
	if (c == '\n') {
		uint8_t cr = '\r';

		wolfSSH_stream_send(ssh, &cr, 1);
	}

	uint8_t ch = (uint8_t)c;

	wolfSSH_stream_send(ssh, &ch, 1);
	return c;
}

static int input_in(void)
{
	WOLFSSH *ssh = g_session;
	uint8_t ch;

	if (ssh == NULL) {
		return -1;
	}

	int n = wolfSSH_stream_read(ssh, &ch, 1);

	if (n <= 0) {
		return -1;
	}
	return (int)ch;
}

/* ── Status output helper — SSH or printk ─────────────────────────────────── */

static void loader_printf(WOLFSSH *ssh, const char *fmt, ...)
{
	char buf[160];
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);

	va_end(ap);
	if (n > 0) {
		if (ssh != NULL) {
			wolfSSH_stream_send(ssh, (uint8_t *)buf, (word32)n);
		} else {
			printk("%s", buf);
		}
	}
}

/* ── Read the whole extension file into a RAM buffer ──────────────────────── */

static int read_file(const char *path, WOLFSSH *ssh, uint8_t **buf_out,
		     size_t *size_out)
{
	struct fs_dirent stat;

	if (fs_stat(path, &stat) != 0) {
		loader_printf(ssh, "Cannot stat '%s'\r\n", path);
		return -1;
	}

	size_t fsize = stat.size;

	if (fsize == 0 || fsize > EXT_MAX_SIZE) {
		loader_printf(ssh, "File size out of range (%lu)\r\n",
			      (unsigned long)fsize);
		return -1;
	}

	uint8_t *buf = k_malloc(fsize);

	if (buf == NULL) {
		loader_printf(ssh, "Out of memory for %lu-byte image\r\n",
			      (unsigned long)fsize);
		return -1;
	}

	struct fs_file_t f;

	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ) != 0) {
		loader_printf(ssh, "Cannot open '%s'\r\n", path);
		k_free(buf);
		return -1;
	}

	ssize_t got = fs_read(&f, buf, fsize);

	fs_close(&f);

	if (got < 0 || (size_t)got != fsize) {
		loader_printf(ssh, "Read failed\r\n");
		k_free(buf);
		return -1;
	}

	*buf_out = buf;
	*size_out = fsize;
	return 0;
}

/* ── Zephyr run thread ────────────────────────────────────────────────────── */

struct ext_run_args {
	ext_entry_fn entry;
	struct k_sem done;
	int result;
};

static K_THREAD_STACK_DEFINE(ext_stack, EXT_STACK_SIZE);
static struct k_thread ext_thread;

static void ext_run_entry(void *p1, void *p2, void *p3)
{
	struct ext_run_args *a = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	a->result = a->entry(0, NULL);
	k_sem_give(&a->done);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void llext_loader_init(void)
{
	k_mutex_init(&g_ext_mutex);
}

int llext_run_from_sd(const char *filename, WOLFSSH *ssh)
{
	k_mutex_lock(&g_ext_mutex, K_FOREVER);

	uint8_t *buf = NULL;
	size_t size = 0;
	int rc = read_file(filename, ssh, &buf, &size);

	if (rc != 0) {
		k_mutex_unlock(&g_ext_mutex);
		return -1;
	}

	loader_printf(ssh, "'%s'  %lu bytes\r\n", filename, (unsigned long)size);

	struct llext_buf_loader buf_loader = LLEXT_BUF_LOADER(buf, size);
	struct llext_loader *ldr = &buf_loader.loader;
	struct llext_load_param ldr_parm = LLEXT_LOAD_PARAM_DEFAULT;
	struct llext *ext = NULL;

	/* Keep ext->sym_tab after load so we can resolve `main` by name. */
	ldr_parm.keep_symtab = true;

	int ret = llext_load(ldr, "run", &ext, &ldr_parm);

	if (ret != 0) {
		loader_printf(ssh, "llext_load failed: %d\r\n", ret);
		k_free(buf);
		k_mutex_unlock(&g_ext_mutex);
		return -2;
	}

	ext_entry_fn entry =
		(ext_entry_fn)llext_find_sym(&ext->sym_tab, "main");

	if (entry == NULL) {
		loader_printf(ssh, "No 'main' symbol in extension\r\n");
		llext_unload(&ext);
		k_free(buf);
		k_mutex_unlock(&g_ext_mutex);
		return -3;
	}

	loader_printf(ssh, "Running main @ %p ...\r\n", (void *)entry);

	struct ext_run_args args;

	args.entry = entry;
	args.result = -1;
	k_sem_init(&args.done, 0, 1);

	/* Install console redirect for the duration of the run. */
	g_session = ssh;
	klausscpu_console_out_hook = ssh ? mirror_out : NULL;
	klausscpu_console_in_hook = ssh ? input_in : NULL;

	k_thread_create(&ext_thread, ext_stack, EXT_STACK_SIZE,
			ext_run_entry, &args, NULL, NULL,
			EXT_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&ext_thread, "ext_run");

	k_sem_take(&args.done, K_FOREVER);

	klausscpu_console_out_hook = NULL;
	klausscpu_console_in_hook = NULL;
	g_session = NULL;

	llext_unload(&ext);
	k_free(buf);
	k_mutex_unlock(&g_ext_mutex);

	return args.result;
}
