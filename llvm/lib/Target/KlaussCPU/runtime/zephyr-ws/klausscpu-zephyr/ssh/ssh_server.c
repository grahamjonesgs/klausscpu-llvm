/* ssh_server.c — wolfSSH server for KlaussCPU Zephyr.
 *
 * Adapted from freertos/ssh/ssh_server.c.  Uses Zephyr k_thread, k_malloc,
 * and BSD socket API (CONFIG_NET_SOCKETS) instead of FreeRTOS + lwIP.
 *
 * The shell provides filesystem commands (ls, cat, mkdir, rm) plus the
 * same hardware-info commands as the FreeRTOS telnet shell.
 */

#define WOLFSSL_USER_SETTINGS
#include "user_settings.h"

#include <zephyr/kernel.h>
#include <zephyr/version.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include <wolfssh/ssh.h>
#include <wolfssl/wolfcrypt/error-crypt.h>

#include "ssh_server.h"
#include "llext_loader.h"
#include "sshkeys.h"
#include "wolfssl_hw.h"

LOG_MODULE_REGISTER(ssh_server, LOG_LEVEL_INF);

/* ── MMIO shortcuts for hardware info commands ───────────────────────────── */

#define REG(a)        (*(volatile uint32_t *)(unsigned long)(a))
#define REG64(a)      (*(volatile uint64_t *)(unsigned long)(a))
#define REG_LEDS      REG(0xF0040000u)
#define REG_SEG_ALL   REG(0xF0030010u)
#define REG_CLOCK_MS  REG64(0xF00F0040u)

/* ── Authentication ──────────────────────────────────────────────────────── */

static ssh_auth_cb_t s_auth_cb;

static int default_auth_cb(const char *user, const char *password,
			   const uint8_t *pubkey, size_t pubkey_len)
{
	(void)pubkey;
	(void)pubkey_len;
	if (strcmp(user, "admin") == 0 && strcmp(password, "klausscpu") == 0) {
		return 0;
	}
	return -1;
}

void ssh_server_set_auth_cb(ssh_auth_cb_t cb)
{
	s_auth_cb = cb;
}

static int wolfssh_auth_cb(uint8_t type, WS_UserAuthData *auth, void *ctx)
{
	(void)ctx;
	ssh_auth_cb_t cb = s_auth_cb ? s_auth_cb : default_auth_cb;

	char user[64];
	word32 usrsz = auth->usernameSz < sizeof(user) - 1
			       ? auth->usernameSz
			       : (word32)(sizeof(user) - 1);
	memcpy(user, auth->username, usrsz);
	user[usrsz] = '\0';

	if (type == WOLFSSH_USERAUTH_PASSWORD) {
		char pw[128];
		word32 pwsz = auth->sf.password.passwordSz < sizeof(pw) - 1
				      ? auth->sf.password.passwordSz
				      : (word32)(sizeof(pw) - 1);
		memcpy(pw, auth->sf.password.password, pwsz);
		pw[pwsz] = '\0';

		int ok = (cb(user, pw, NULL, 0) == 0);

		LOG_INF("auth user='%s' password: %s", user,
			ok ? "OK" : "FAIL");
		return ok ? WOLFSSH_USERAUTH_SUCCESS
			  : WOLFSSH_USERAUTH_FAILURE;
	}
	if (type == WOLFSSH_USERAUTH_PUBLICKEY) {
		const uint8_t *pk = auth->sf.publicKey.publicKey;
		word32 pk_len = auth->sf.publicKey.publicKeySz;
		int ok = (cb(user, NULL, pk, pk_len) == 0);

		LOG_INF("auth user='%s' pubkey: %s", user,
			ok ? "OK" : "FAIL");
		return ok ? WOLFSSH_USERAUTH_SUCCESS
			  : WOLFSSH_USERAUTH_FAILURE;
	}
	return WOLFSSH_USERAUTH_FAILURE;
}

/* ── Socket I/O callbacks for wolfSSH ────────────────────────────────────── */

static int ssh_recv(WOLFSSH *ssh, void *buf, word32 sz, void *ctx)
{
	int sock = *(int *)ctx;
	int n = zsock_recv(sock, buf, sz, 0);

	if (n == 0) {
		return WS_CBIO_ERR_CONN_CLOSE;
	}
	if (n < 0) {
		return WS_CBIO_ERR_GENERAL;
	}
	return n;
}

static int ssh_send(WOLFSSH *ssh, void *buf, word32 sz, void *ctx)
{
	int sock = *(int *)ctx;
	int n = zsock_send(sock, buf, sz, 0);

	if (n < 0) {
		return WS_CBIO_ERR_GENERAL;
	}
	return n;
}

/* ── Shell output helpers ────────────────────────────────────────────────── */

#define SSH_VERSION  "1.3"   /* 1.3: command history (up/down arrows) */
#define SSH_BANNER   "\r\nKlaussCPU Zephyr SSH shell v" SSH_VERSION \
		     " (built " __DATE__ " " __TIME__ ") — type 'help'\r\n\r\n"
#define SSH_PROMPT   "$ "
#define SSH_LINE_MAX 256
#define SSH_HIST_SIZE 8       /* command-history ring depth (per session) */
#define SSH_HIST_LINE 128     /* max stored command length */

static void ssh_send_str(WOLFSSH *ssh, const char *s)
{
	wolfSSH_stream_send(ssh, (uint8_t *)s, (word32)strlen(s));
}

static void ssh_sendf(WOLFSSH *ssh, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0) {
		wolfSSH_stream_send(ssh, (uint8_t *)buf, (word32)n);
	}
}

/* ── Path helper — prepend /SD:/ to relative paths ───────────────────────── */

#define SD_ROOT "/SD:/"

static char _pathbuf[128];

static const char *resolve_path(const char *path)
{
	if (!path || !*path) {
		return SD_ROOT;
	}
	if (path[0] == '/') {
		return path;
	}
	snprintf(_pathbuf, sizeof(_pathbuf), SD_ROOT "%s", path);
	return _pathbuf;
}

/* ── Shell commands ──────────────────────────────────────────────────────── */

static void cmd_help(WOLFSSH *ssh)
{
	ssh_send_str(ssh,
		"Commands:\r\n"
		"  help           this message\r\n"
		"  info           system information\r\n"
		"  uptime         system uptime\r\n"
		"  threads        list kernel threads\r\n"
		"  ls [path]      list directory (default /SD:/)\r\n"
		"  cat <file>     display file contents\r\n"
		"  hexdump <file> hex dump of file\r\n"
		"  mkdir <path>   create directory\r\n"
		"  rm <file>      remove file\r\n"
		"  write <file> <text>  write text to file\r\n"
		"  df             filesystem free space\r\n"
		"  run [file]     load & run extension (default PROG.LLEXT)\r\n"
		"  leds [hex]     read/write LED register\r\n"
		"  seg <hex>      write 7-segment display\r\n"
		"  perf [ms]      profile live CPU/cache (default 1000ms window)\r\n"
		"  exit           close SSH session\r\n");
}

static void cmd_info(WOLFSSH *ssh)
{
	ssh_sendf(ssh, "KlaussCPU Zephyr %s\r\n", KERNEL_VERSION_STRING);
	ssh_sendf(ssh, "Uptime : %llu ms\r\n",
		  (unsigned long long)k_uptime_get());
	ssh_sendf(ssh, "Clock  : %u Hz\r\n",
		  CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC);
}

/* ── Live performance counters — `perf [ms]` ─────────────────────────────── */
/* Delta-samples the MMIO perf (0xF00D) + cache (0xF005) counters over a window
 * (non-destructive: read, sleep, read, subtract) and reports what the live
 * system did — CPI, busy/idle, cycle accounting, instruction mix, cache. */

struct perfsnap {
	uint64_t cycles, instr, fetch, exec, mul, div, intc, idle;
	uint64_t mul_ops, div_ops, int_ops;
	uint64_t alu, load, store, branch, taken, jump, call, ind, other;
	uint64_t rh, rm, wh, wm, wb, stall;
};

static void perf_read(struct perfsnap *s)
{
	s->cycles  = REG64(0xF00D0008u); s->instr = REG64(0xF00D0010u);
	s->fetch   = REG64(0xF00D0018u); s->exec  = REG64(0xF00D0020u);
	s->mul     = REG64(0xF00D0028u); s->div   = REG64(0xF00D0030u);
	s->intc    = REG64(0xF00D0038u); s->idle  = REG64(0xF00D0040u);
	s->mul_ops = REG64(0xF00D0048u); s->div_ops = REG64(0xF00D0050u);
	s->int_ops = REG64(0xF00D0058u);
	s->alu     = REG64(0xF00D0060u); s->load  = REG64(0xF00D0068u);
	s->store   = REG64(0xF00D0070u); s->branch = REG64(0xF00D0078u);
	s->taken   = REG64(0xF00D0080u); s->jump  = REG64(0xF00D0088u);
	s->call    = REG64(0xF00D0090u); s->ind   = REG64(0xF00D0098u);
	s->other   = REG64(0xF00D00A0u);
	s->rh = REG64(0xF0050040u); s->rm = REG64(0xF0050048u);
	s->wh = REG64(0xF0050050u); s->wm = REG64(0xF0050058u);
	s->wb = REG64(0xF0050060u); s->stall = REG64(0xF0050068u);
}

static void ssh_pct(WOLFSSH *ssh, const char *label, uint64_t num, uint64_t den)
{
	uint64_t p = den ? (num * 10000ull) / den : 0;   /* hundredths of % */

	ssh_sendf(ssh, "%s%llu.%02llu%% ", label,
		  (unsigned long long)(p / 100), (unsigned long long)(p % 100));
}

static void cmd_perf(WOLFSSH *ssh, const char *arg)
{
	uint32_t ms = arg ? (uint32_t)strtoul(arg, NULL, 10) : 1000u;

	if (ms < 10u)    ms = 10u;
	if (ms > 60000u) ms = 60000u;

	struct perfsnap a, b;

	perf_read(&a);
	k_msleep((int32_t)ms);
	perf_read(&b);

#define DLT(f) ((uint64_t)(b.f - a.f))
	uint64_t cyc = DLT(cycles), ins = DLT(instr), idle = DLT(idle);
	uint64_t misses = DLT(rm) + DLT(wm);
	uint64_t acc = DLT(rh) + DLT(rm) + DLT(wh) + DLT(wm);

	ssh_sendf(ssh, "perf window=%llums  cycles=%llu  instr=%llu\r\n",
		  (unsigned long long)ms, (unsigned long long)cyc,
		  (unsigned long long)ins);
	if (ins) {
		uint64_t cpi = (cyc * 1000ull) / ins;

		ssh_sendf(ssh, "CPI=%llu.%03llu\r\n",
			  (unsigned long long)(cpi / 1000),
			  (unsigned long long)(cpi % 1000));
	} else {
		ssh_send_str(ssh, "CPI=n/a (idle)\r\n");
	}

	ssh_send_str(ssh, "util: busy=");
	ssh_pct(ssh, "", cyc - idle, cyc);
	ssh_pct(ssh, "idle=", idle, cyc);
	ssh_send_str(ssh, "\r\n");

	ssh_send_str(ssh, "cyc:  ");
	ssh_pct(ssh, "fetch=", DLT(fetch), cyc);
	ssh_pct(ssh, "exec=",  DLT(exec),  cyc);
	ssh_pct(ssh, "mul=",   DLT(mul),   cyc);
	ssh_pct(ssh, "div=",   DLT(div),   cyc);
	ssh_pct(ssh, "int=",   DLT(intc),  cyc);
	ssh_pct(ssh, "idle=",  idle,       cyc);
	ssh_send_str(ssh, "\r\n");

	ssh_send_str(ssh, "mix:  ");
	ssh_pct(ssh, "ALU=",  DLT(alu),    ins);
	ssh_pct(ssh, "LD=",   DLT(load),   ins);
	ssh_pct(ssh, "ST=",   DLT(store),  ins);
	ssh_pct(ssh, "BR=",   DLT(branch), ins);
	ssh_pct(ssh, "JMP=",  DLT(jump),   ins);
	ssh_pct(ssh, "CALL=", DLT(call),   ins);
	ssh_pct(ssh, "IND=",  DLT(ind),    ins);
	ssh_pct(ssh, "OTH=",  DLT(other),  ins);
	ssh_send_str(ssh, "\r\n");

	uint64_t br = DLT(branch), tk = DLT(taken);

	ssh_sendf(ssh, "branch: n=%llu taken=%llu rate=",
		  (unsigned long long)br, (unsigned long long)tk);
	ssh_pct(ssh, "", tk, br);
	ssh_send_str(ssh, "\r\n");

	ssh_sendf(ssh, "cache: acc=%llu miss=", (unsigned long long)acc);
	ssh_pct(ssh, "", misses, acc);
	ssh_sendf(ssh, "wb=%llu stall=%llu",
		  (unsigned long long)DLT(wb), (unsigned long long)DLT(stall));
	if (misses) {
		uint64_t pen = (DLT(stall) * 100ull) / misses;

		ssh_sendf(ssh, " avgpen=%llu.%02lluc",
			  (unsigned long long)(pen / 100),
			  (unsigned long long)(pen % 100));
	}
	ssh_send_str(ssh, "\r\n");
#undef DLT
}

static void cmd_uptime(WOLFSSH *ssh)
{
	int64_t ms = k_uptime_get();
	int64_t sec = ms / 1000;
	int64_t min = sec / 60;
	int64_t hr = min / 60;

	ssh_sendf(ssh, "%02lld:%02lld:%02lld.%03lld\r\n",
		  (long long)hr, (long long)(min % 60),
		  (long long)(sec % 60), (long long)(ms % 1000));
}

static void cmd_ls(WOLFSSH *ssh, const char *path)
{
	struct fs_dir_t dir;
	struct fs_dirent entry;

	path = resolve_path(path);

	fs_dir_t_init(&dir);

	if (fs_opendir(&dir, path) != 0) {
		ssh_sendf(ssh, "Cannot open directory: %s\r\n", path);
		return;
	}

	ssh_sendf(ssh, "Directory: %s\r\n", path);

	while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
		if (entry.type == FS_DIR_ENTRY_DIR) {
			ssh_sendf(ssh, "  [DIR]  %s\r\n", entry.name);
		} else {
			ssh_sendf(ssh, "  %6u  %s\r\n",
				  (unsigned)entry.size, entry.name);
		}
	}
	fs_closedir(&dir);
}

static void cmd_cat(WOLFSSH *ssh, const char *path)
{
	struct fs_file_t f;
	uint8_t buf[256];

	if (!path || !*path) {
		ssh_send_str(ssh, "Usage: cat <file>\r\n");
		return;
	}
	path = resolve_path(path);

	fs_file_t_init(&f);

	if (fs_open(&f, path, FS_O_READ) != 0) {
		ssh_sendf(ssh, "Cannot open: %s\r\n", path);
		return;
	}

	ssize_t n;

	while ((n = fs_read(&f, buf, sizeof(buf))) > 0) {
		/* Convert \n to \r\n for terminal display */
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] == '\n') {
				ssh_send_str(ssh, "\r\n");
			} else {
				wolfSSH_stream_send(ssh, &buf[i], 1);
			}
		}
	}
	fs_close(&f);
	ssh_send_str(ssh, "\r\n");
}

static void cmd_hexdump(WOLFSSH *ssh, const char *path)
{
	struct fs_file_t f;
	uint8_t buf[16];

	if (!path || !*path) {
		ssh_send_str(ssh, "Usage: hexdump <file>\r\n");
		return;
	}
	path = resolve_path(path);

	fs_file_t_init(&f);

	if (fs_open(&f, path, FS_O_READ) != 0) {
		ssh_sendf(ssh, "Cannot open: %s\r\n", path);
		return;
	}

	uint32_t offset = 0;
	ssize_t n;

	while ((n = fs_read(&f, buf, sizeof(buf))) > 0) {
		ssh_sendf(ssh, "%08x  ", offset);
		for (ssize_t i = 0; i < n; i++) {
			ssh_sendf(ssh, "%02x ", buf[i]);
		}
		for (ssize_t i = n; i < 16; i++) {
			ssh_send_str(ssh, "   ");
		}
		ssh_send_str(ssh, " |");
		for (ssize_t i = 0; i < n; i++) {
			char c = (buf[i] >= 0x20 && buf[i] < 0x7f) ? (char)buf[i]
								    : '.';
			wolfSSH_stream_send(ssh, (uint8_t *)&c, 1);
		}
		ssh_send_str(ssh, "|\r\n");
		offset += (uint32_t)n;
	}
	fs_close(&f);
	ssh_sendf(ssh, "%08x  (%u bytes)\r\n", offset, offset);
}

static void cmd_mkdir(WOLFSSH *ssh, const char *path)
{
	if (!path || !*path) {
		ssh_send_str(ssh, "Usage: mkdir <path>\r\n");
		return;
	}
	path = resolve_path(path);
	if (fs_mkdir(path) != 0) {
		ssh_sendf(ssh, "Failed to create: %s\r\n", path);
	} else {
		ssh_sendf(ssh, "Created: %s\r\n", path);
	}
}

static void cmd_rm(WOLFSSH *ssh, const char *path)
{
	if (!path || !*path) {
		ssh_send_str(ssh, "Usage: rm <file>\r\n");
		return;
	}
	path = resolve_path(path);
	if (fs_unlink(path) != 0) {
		ssh_sendf(ssh, "Failed to remove: %s\r\n", path);
	} else {
		ssh_sendf(ssh, "Removed: %s\r\n", path);
	}
}

static void cmd_write(WOLFSSH *ssh, const char *path, const char *text)
{
	struct fs_file_t f;

	if (!path || !*path || !text) {
		ssh_send_str(ssh, "Usage: write <file> <text>\r\n");
		return;
	}
	path = resolve_path(path);

	fs_file_t_init(&f);

	if (fs_open(&f, path, FS_O_WRITE | FS_O_CREATE) != 0) {
		ssh_sendf(ssh, "Cannot open: %s\r\n", path);
		return;
	}

	ssize_t bw = fs_write(&f, text, strlen(text));

	fs_close(&f);

	if (bw >= 0) {
		ssh_sendf(ssh, "Wrote %d bytes to %s\r\n", (int)bw, path);
	} else {
		ssh_sendf(ssh, "Write failed: %d\r\n", (int)bw);
	}
}

static void cmd_df(WOLFSSH *ssh)
{
	struct fs_statvfs stat;

	if (fs_statvfs("/SD:/", &stat) != 0) {
		ssh_send_str(ssh, "Cannot stat filesystem\r\n");
		return;
	}

	uint64_t total = (uint64_t)stat.f_bsize * stat.f_blocks;
	uint64_t free_bytes = (uint64_t)stat.f_bsize * stat.f_bfree;

	ssh_sendf(ssh, "Filesystem: /SD:/\r\n");
	ssh_sendf(ssh, "  Block size : %lu\r\n", (unsigned long)stat.f_bsize);
	ssh_sendf(ssh, "  Total      : %llu bytes\r\n",
		  (unsigned long long)total);
	ssh_sendf(ssh, "  Free       : %llu bytes\r\n",
		  (unsigned long long)free_bytes);
}

static void cmd_leds(WOLFSSH *ssh, const char *arg)
{
	if (arg && *arg) {
		unsigned long val = strtoul(arg, NULL, 16);

		REG_LEDS = (uint32_t)val;
		ssh_sendf(ssh, "LEDs = 0x%04lx\r\n", val & 0xFFFF);
	} else {
		ssh_sendf(ssh, "LEDs = 0x%04lx\r\n",
			  (unsigned long)(REG_LEDS & 0xFFFF));
	}
}

static void cmd_seg(WOLFSSH *ssh, const char *arg)
{
	if (!arg || !*arg) {
		ssh_send_str(ssh, "Usage: seg <hex32>\r\n");
		return;
	}
	unsigned long val = strtoul(arg, NULL, 16);

	REG_SEG_ALL = (uint32_t)val;
	ssh_sendf(ssh, "7-seg = 0x%08lx\r\n", val);
}

static void cmd_run(WOLFSSH *ssh, const char *arg)
{
	const char *filename;
	char pathbuf[128];

	if (arg && *arg) {
		if (arg[0] == '/') {
			filename = arg;
		} else {
			snprintf(pathbuf, sizeof(pathbuf), "/SD:/%s", arg);
			filename = pathbuf;
		}
	} else {
		filename = "/SD:/PROG.LLEXT";
	}

	ssh_sendf(ssh, "Loading %s ...\r\n", filename);
	int rc = llext_run_from_sd(filename, ssh);

	if (rc < 0) {
		ssh_sendf(ssh, "\r\nLoad/run failed (code %d)\r\n", rc);
	} else {
		ssh_sendf(ssh, "\r\nProgram exited: %d\r\n", rc);
	}
}

struct thread_cb_ctx {
	WOLFSSH *ssh;
};

#ifdef CONFIG_THREAD_MONITOR
static void thread_print_cb(const struct k_thread *t, void *ctx)
{
	struct thread_cb_ctx *c = ctx;
	const char *name = k_thread_name_get((k_tid_t)t);

	if (!name) {
		name = "(unnamed)";
	}
	ssh_sendf(c->ssh, "  %s  prio=%d\r\n", name,
		  k_thread_priority_get((k_tid_t)t));
}
#endif

static void cmd_threads(WOLFSSH *ssh)
{
	ssh_send_str(ssh, "Active threads:\r\n");
#ifdef CONFIG_THREAD_MONITOR
	struct thread_cb_ctx ctx = { .ssh = ssh };

	k_thread_foreach(thread_print_cb, &ctx);
#else
	ssh_send_str(ssh, "  (enable CONFIG_THREAD_MONITOR)\r\n");
#endif
}

/* ── Command dispatcher ──────────────────────────────────────────────────── */

static int dispatch_command(WOLFSSH *ssh, char *line)
{
	char *p = line;

	while (*p == ' ') {
		p++;
	}
	if (*p == '\0') {
		return 0;
	}

	char *arg = p;

	while (*arg && *arg != ' ') {
		arg++;
	}
	if (*arg == ' ') {
		*arg++ = '\0';
		while (*arg == ' ') {
			arg++;
		}
	}
	if (!*arg) {
		arg = NULL;
	}

	/* For 'write' command, get second argument (text after filename) */
	char *arg2 = NULL;

	if (arg && strcmp(p, "write") == 0) {
		arg2 = arg;
		while (*arg2 && *arg2 != ' ') {
			arg2++;
		}
		if (*arg2 == ' ') {
			*arg2++ = '\0';
			while (*arg2 == ' ') {
				arg2++;
			}
		}
		if (!*arg2) {
			arg2 = NULL;
		}
	}

	if (strcmp(p, "help") == 0) {
		cmd_help(ssh);
	} else if (strcmp(p, "info") == 0) {
		cmd_info(ssh);
	} else if (strcmp(p, "uptime") == 0) {
		cmd_uptime(ssh);
	} else if (strcmp(p, "ls") == 0) {
		cmd_ls(ssh, arg);
	} else if (strcmp(p, "cat") == 0) {
		cmd_cat(ssh, arg);
	} else if (strcmp(p, "hexdump") == 0) {
		cmd_hexdump(ssh, arg);
	} else if (strcmp(p, "mkdir") == 0) {
		cmd_mkdir(ssh, arg);
	} else if (strcmp(p, "rm") == 0) {
		cmd_rm(ssh, arg);
	} else if (strcmp(p, "write") == 0) {
		cmd_write(ssh, arg, arg2);
	} else if (strcmp(p, "df") == 0) {
		cmd_df(ssh);
	} else if (strcmp(p, "run") == 0) {
		cmd_run(ssh, arg);
	} else if (strcmp(p, "leds") == 0) {
		cmd_leds(ssh, arg);
	} else if (strcmp(p, "seg") == 0) {
		cmd_seg(ssh, arg);
	} else if (strcmp(p, "perf") == 0) {
		cmd_perf(ssh, arg);
	} else if (strcmp(p, "threads") == 0) {
		cmd_threads(ssh);
	} else if (strcmp(p, "exit") == 0 || strcmp(p, "quit") == 0 ||
		   strcmp(p, "logout") == 0) {
		ssh_send_str(ssh, "Bye.\r\n");
		return 1;
	} else {
		ssh_sendf(ssh, "Unknown command: %s  (type 'help')\r\n", p);
	}
	return 0;
}

/* ── Interactive shell loop ──────────────────────────────────────────────── */

/* Replace the on-screen line with `line` (used for history recall). */
static void shell_redraw(WOLFSSH *ssh, const char *line, int len)
{
	ssh_send_str(ssh, "\r\x1b[K");          /* CR + erase to end of line */
	ssh_send_str(ssh, SSH_PROMPT);
	if (len > 0) {
		wolfSSH_stream_send(ssh, (uint8_t *)line, (word32)len);
	}
}

static void run_shell(WOLFSSH *ssh)
{
	char line[SSH_LINE_MAX];
	int line_len = 0;

	/* Per-session command history (ring). */
	char hist[SSH_HIST_SIZE][SSH_HIST_LINE];
	int hist_head = 0;   /* next slot to write                    */
	int hist_n = 0;      /* entries stored (<= SSH_HIST_SIZE)      */
	int browse = -1;     /* -1 = new line; 0 = newest, up = older  */
	int esc = 0;         /* 0 none, 1 saw ESC, 2 saw ESC[          */

	ssh_send_str(ssh, SSH_BANNER);
	ssh_send_str(ssh, SSH_PROMPT);

	uint8_t ch;

	while (1) {
		int n = wolfSSH_stream_read(ssh, &ch, 1);

		if (n <= 0) {
			break;
		}

		/* ── arrow-key / escape-sequence handling (ESC '[' 'A'/'B') ── */
		if (esc == 1) {
			esc = (ch == '[') ? 2 : 0;
			continue;
		}
		if (esc == 2) {
			esc = 0;
			if (ch == 'A' && browse + 1 < hist_n) {
				browse++;            /* older */
			} else if (ch == 'B' && browse > 0) {
				browse--;            /* newer */
			} else if (ch == 'B' && browse == 0) {
				browse = -1;         /* back to a fresh empty line */
				line_len = 0;
				shell_redraw(ssh, line, 0);
				continue;
			} else {
				continue;            /* at end of history, or C/D */
			}
			/* load history entry `browse` (0 = newest) into `line` */
			int idx = (hist_head - 1 - browse + 2 * SSH_HIST_SIZE) % SSH_HIST_SIZE;
			size_t hl = strlen(hist[idx]);

			if (hl > SSH_LINE_MAX - 1) {
				hl = SSH_LINE_MAX - 1;
			}
			memcpy(line, hist[idx], hl);
			line_len = (int)hl;
			line[line_len] = '\0';
			shell_redraw(ssh, line, line_len);
			continue;
		}
		if (ch == 0x1B) {
			esc = 1;
			continue;
		}

		/* ── normal line editing ── */
		if (ch == '\r' || ch == '\n') {
			ssh_send_str(ssh, "\r\n");
			line[line_len] = '\0';

			/* store non-empty command in history */
			if (line_len > 0) {
				size_t cl = (size_t)line_len;

				if (cl > SSH_HIST_LINE - 1) {
					cl = SSH_HIST_LINE - 1;
				}
				memcpy(hist[hist_head], line, cl);
				hist[hist_head][cl] = '\0';
				hist_head = (hist_head + 1) % SSH_HIST_SIZE;
				if (hist_n < SSH_HIST_SIZE) {
					hist_n++;
				}
			}
			browse = -1;

			if (dispatch_command(ssh, line) != 0) {
				break;
			}

			line_len = 0;
			ssh_send_str(ssh, SSH_PROMPT);
		} else if (ch == 127 || ch == 8) {
			if (line_len > 0) {
				line_len--;
				ssh_send_str(ssh, "\b \b");
			}
		} else if (ch == 3) {
			/* Ctrl-C: clear line */
			while (line_len > 0) {
				ssh_send_str(ssh, "\b \b");
				line_len--;
			}
			browse = -1;
		} else if (ch == 4) {
			/* Ctrl-D: disconnect */
			ssh_send_str(ssh, "\r\nBye.\r\n");
			break;
		} else if (ch >= 0x20 && line_len < SSH_LINE_MAX - 1) {
			line[line_len++] = (char)ch;
			wolfSSH_stream_send(ssh, &ch, 1);
		}
	}
}

/* ── Connection handler thread ───────────────────────────────────────────── */

struct conn_args {
	WOLFSSH *ssh;
	int sock;
};

/* The connection thread also runs loaded extensions' main() in-place (see
 * llext_loader.c), so it needs room for the shell frame plus the program. */
#define CONN_STACK_SIZE 40960

static K_THREAD_STACK_ARRAY_DEFINE(conn_stacks, SSH_MAX_CONNS, CONN_STACK_SIZE);
static struct k_thread conn_threads[SSH_MAX_CONNS];
static struct conn_args conn_args_pool[SSH_MAX_CONNS];
static bool conn_busy[SSH_MAX_CONNS];
static struct k_mutex conn_mutex;

static void ssh_conn_entry(void *p1, void *p2, void *p3)
{
	struct conn_args *ca = p1;
	int slot = (int)(uintptr_t)p2;

	ARG_UNUSED(p3);

	WOLFSSH *ssh = ca->ssh;
	int sock = ca->sock;

	wolfSSH_SetIOReadCtx(ssh, &sock);
	wolfSSH_SetIOWriteCtx(ssh, &sock);

	int rc = wolfSSH_accept(ssh);

	if (rc != WS_SUCCESS) {
		int err = wolfSSH_get_error(ssh);

		LOG_ERR("accept failed rc=%d err=%d", rc, err);
		goto cleanup;
	}

	LOG_INF("SSH session established");
	run_shell(ssh);

cleanup:
	wolfSSH_free(ssh);
	zsock_close(sock);
	LOG_INF("SSH session ended");

	k_mutex_lock(&conn_mutex, K_FOREVER);
	conn_busy[slot] = false;
	k_mutex_unlock(&conn_mutex);
}

/* ── Listener thread ─────────────────────────────────────────────────────── */

static WOLFSSH_CTX *s_ctx;

#define LISTEN_STACK_SIZE 4096
static K_THREAD_STACK_DEFINE(listen_stack, LISTEN_STACK_SIZE);
static struct k_thread listen_thread;

static void ssh_listener_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	sshkeys_persist();

	int listen_sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (listen_sock < 0) {
		LOG_ERR("socket() failed");
		return;
	}

	int yes = 1;

	zsock_setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &yes,
			 sizeof(yes));

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(SSH_PORT),
		.sin_addr = {.s_addr = INADDR_ANY},
	};

	if (zsock_bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) <
		    0 ||
	    zsock_listen(listen_sock, SSH_MAX_CONNS) < 0) {
		LOG_ERR("bind/listen failed");
		zsock_close(listen_sock);
		return;
	}
	LOG_INF("SSH listening on port %d", SSH_PORT);

	while (1) {
		struct sockaddr_in client_addr;
		socklen_t addr_len = sizeof(client_addr);
		int client = zsock_accept(listen_sock,
					  (struct sockaddr *)&client_addr,
					  &addr_len);

		if (client < 0) {
			continue;
		}

		LOG_INF("SSH connection accepted");

		/* Find a free connection slot */
		int slot = -1;

		k_mutex_lock(&conn_mutex, K_FOREVER);
		for (int i = 0; i < SSH_MAX_CONNS; i++) {
			if (!conn_busy[i]) {
				conn_busy[i] = true;
				slot = i;
				break;
			}
		}
		k_mutex_unlock(&conn_mutex);

		if (slot < 0) {
			LOG_WRN("Max connections reached, rejecting");
			zsock_close(client);
			continue;
		}

		WOLFSSH *ssh = wolfSSH_new(s_ctx);

		if (!ssh) {
			LOG_ERR("wolfSSH_new() failed");
			zsock_close(client);
			k_mutex_lock(&conn_mutex, K_FOREVER);
			conn_busy[slot] = false;
			k_mutex_unlock(&conn_mutex);
			continue;
		}

		conn_args_pool[slot].ssh = ssh;
		conn_args_pool[slot].sock = client;

		k_thread_create(&conn_threads[slot], conn_stacks[slot],
				CONN_STACK_SIZE, ssh_conn_entry,
				&conn_args_pool[slot],
				(void *)(uintptr_t)slot, NULL, SSH_PRIO, 0,
				K_NO_WAIT);

		char tname[16];

		snprintf(tname, sizeof(tname), "sshc%d", slot);
		k_thread_name_set(&conn_threads[slot], tname);
	}
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int ssh_server_start(void)
{
	k_mutex_init(&conn_mutex);
	llext_loader_init();

	if (wolfSSH_Init() != WS_SUCCESS) {
		LOG_ERR("wolfSSH_Init failed");
		return -1;
	}

	if (wolfssl_hw_init() != 0) {
		LOG_WRN("HW crypto init failed — using software");
	}

	if (sshkeys_init() != 0) {
		LOG_ERR("Host key init failed");
		return -1;
	}

	s_ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL);
	if (!s_ctx) {
		LOG_ERR("wolfSSH_CTX_new failed");
		return -1;
	}

	wolfSSH_SetIORecv(s_ctx, ssh_recv);
	wolfSSH_SetIOSend(s_ctx, ssh_send);
	wolfSSH_SetUserAuth(s_ctx, wolfssh_auth_cb);

	const uint8_t *priv = sshkeys_get_private();
	size_t priv_len = sshkeys_get_private_len();

	if (wolfSSH_CTX_UsePrivateKey_buffer(s_ctx, priv, (word32)priv_len,
					     WOLFSSH_FORMAT_ASN1) !=
	    WS_SUCCESS) {
		LOG_ERR("UsePrivateKey failed");
		wolfSSH_CTX_free(s_ctx);
		s_ctx = NULL;
		return -1;
	}

	k_thread_create(&listen_thread, listen_stack, LISTEN_STACK_SIZE,
			ssh_listener_entry, NULL, NULL, NULL, SSH_PRIO, 0,
			K_NO_WAIT);
	k_thread_name_set(&listen_thread, "ssh_listen");

	LOG_INF("SSH server started");
	return 0;
}
