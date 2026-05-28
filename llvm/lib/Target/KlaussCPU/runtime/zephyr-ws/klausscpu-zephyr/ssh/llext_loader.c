/* llext_loader.c — Load and run an LLEXT extension from the SD card,
 *                  redirecting the extension's stdio to an SSH session.
 *
 * Replaces the custom PIC loader (pic_loader.c).  Extensions are ELFCLASS32
 * ET_REL objects produced by the KlaussCPU clang with `-c`; they call the
 * kernel's exported libc (see llext_exports.c) rather than bundling their own.
 *
 * Concurrency: the extension's `main` runs ON THE CALLING (per-connection) SSH
 * thread, and stdio is routed by the *currently running thread* via thread
 * custom data.  Because every SSH session already has its own connection thread
 * (conn_threads[SSH_MAX_CONNS]), several extensions run concurrently, each with
 * its stdout/stdin bound to its own session.  llext_load()/llext_unload() are
 * internally serialised by the subsystem's llext_lock, so concurrent loads are
 * safe; nothing here holds a global lock across a run.
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

/* Console redirect hooks (declared in arch/klausscpu/core/irq.c and
 * ssh/llext_exports.c).  Set once in llext_loader_init(); each callback then
 * keys off the running thread, so they need no per-run mutation. */
extern int (*klausscpu_console_out_hook)(int c);
extern int (*klausscpu_console_in_hook)(void);

typedef int (*ext_entry_fn)(int argc, char **argv);

/* Per-run I/O context, pointed to by the running thread's custom data while its
 * main() executes.  Lives on that thread's stack for the duration of the run. */
struct ext_ctx {
	WOLFSSH *ssh;
};

/* ── Console routing (run on the extension's own thread) ──────────────────── */

/* Output router for arch_printk_char_out(): returns 1 if it sent the char to
 * the current thread's SSH session, 0 to let it fall through to the UART. */
static int route_out(int c)
{
	struct ext_ctx *ctx = k_thread_custom_data_get();

	if (ctx == NULL || ctx->ssh == NULL) {
		return 0;
	}

	WOLFSSH *ssh = ctx->ssh;

	if (c == '\n') {
		uint8_t cr = '\r';

		wolfSSH_stream_send(ssh, &cr, 1);
	}

	uint8_t ch = (uint8_t)c;

	wolfSSH_stream_send(ssh, &ch, 1);
	return 1;
}

/* Input source for the exported getchar(): reads one byte from the current
 * thread's SSH session, or -1 (EOF) if this thread is not running an extension. */
static int route_in(void)
{
	struct ext_ctx *ctx = k_thread_custom_data_get();

	if (ctx == NULL || ctx->ssh == NULL) {
		return -1;
	}

	uint8_t ch;
	int n = wolfSSH_stream_read(ctx->ssh, &ch, 1);

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

/* ── Public API ───────────────────────────────────────────────────────────── */

void llext_loader_init(void)
{
	/* Install the per-thread console routers once; they are no-ops on
	 * threads that are not currently running an extension. */
	klausscpu_console_out_hook = route_out;
	klausscpu_console_in_hook = route_in;
}

int llext_run_from_sd(const char *filename, WOLFSSH *ssh)
{
	uint8_t *buf = NULL;
	size_t size = 0;

	if (read_file(filename, ssh, &buf, &size) != 0) {
		return -1;
	}

	loader_printf(ssh, "'%s'  %lu bytes\r\n", filename, (unsigned long)size);

	struct llext_buf_loader buf_loader = LLEXT_BUF_LOADER(buf, size);
	struct llext_loader *ldr = &buf_loader.loader;
	struct llext_load_param ldr_parm = LLEXT_LOAD_PARAM_DEFAULT;
	struct llext *ext = NULL;

	/* Keep ext->sym_tab after load so we can resolve `main` by name. */
	ldr_parm.keep_symtab = true;

	/* llext_load() deduplicates by name (a second load of the same name just
	 * bumps a use count and returns the existing extension).  We always want a
	 * fresh, independent instance — even of the same program in another session
	 * — so give every load a unique name. */
	static atomic_t ext_seq;
	char ext_name[16];

	snprintf(ext_name, sizeof(ext_name), "ext%u",
		 (unsigned int)atomic_inc(&ext_seq));

	int ret = llext_load(ldr, ext_name, &ext, &ldr_parm);

	if (ret != 0) {
		loader_printf(ssh, "llext_load failed: %d\r\n", ret);
		k_free(buf);
		return -2;
	}

	ext_entry_fn entry =
		(ext_entry_fn)llext_find_sym(&ext->sym_tab, "main");

	if (entry == NULL) {
		loader_printf(ssh, "No 'main' symbol in extension\r\n");
		llext_unload(&ext);
		k_free(buf);
		return -3;
	}

	loader_printf(ssh, "Running main @ %p ...\r\n", (void *)entry);

	/* Route this thread's stdout/stdin to the SSH session while main() runs,
	 * then restore.  Running on the connection thread (rather than a shared
	 * worker) is what lets multiple sessions run extensions concurrently. */
	struct ext_ctx ctx = { .ssh = ssh };
	void *prev = k_thread_custom_data_get();

	k_thread_custom_data_set(&ctx);
	int result = entry(0, NULL);

	k_thread_custom_data_set(prev);

	llext_unload(&ext);
	k_free(buf);

	return result;
}
