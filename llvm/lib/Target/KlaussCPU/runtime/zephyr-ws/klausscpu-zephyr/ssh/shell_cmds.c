/* shell_cmds.c — Zephyr shell commands for KlaussCPU.
 *
 * Registers filesystem and hardware commands with the Zephyr shell subsystem
 * so they are available on the UART console (and can be forwarded to SSH).
 *
 * These commands work independently of the SSH server — they use printk/shell
 * output on the serial console.
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/fs/fs.h>
#include <zephyr/version.h>
#include <zephyr/sys/iterable_sections.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell_paths.h"

/* MMIO shortcuts */
#define REG(a)        (*(volatile uint32_t *)(unsigned long)(a))
#define REG64(a)      (*(volatile uint64_t *)(unsigned long)(a))
#define REG_LEDS      REG(0xF0040000u)
#define REG_SWITCHES  REG(0xF0040008u)
#define REG_SEG_ALL   REG(0xF0030010u)

/* ── Filesystem commands ─────────────────────────────────────────────────── */

static int cmd_ls(const struct shell *sh, size_t argc, char **argv)
{
	char path[SHELL_PATH_MAX];
	struct fs_dir_t dir;
	struct fs_dirent entry;

	if (shell_redir_begin(sh, &argc, argv) < 0) {
		shell_error(sh, "ls: cannot open redirect target");
		return -1;
	}

	shell_path_resolve(sh, (argc > 1) ? argv[1] : NULL, path, sizeof(path));
	fs_dir_t_init(&dir);

	if (fs_opendir(&dir, path) != 0) {
		shell_error(sh, "Cannot open directory: %s", path);
		shell_redir_end(sh);
		return -1;
	}

	sh_print(sh, "Directory: %s", path);

	while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
		if (entry.type == FS_DIR_ENTRY_DIR) {
			sh_print(sh, "  [DIR]  %s", entry.name);
		} else {
			sh_print(sh, "  %6u  %s", (unsigned)entry.size,
				 entry.name);
		}
	}
	fs_closedir(&dir);
	shell_redir_end(sh);
	return 0;
}

static int cmd_cat(const struct shell *sh, size_t argc, char **argv)
{
	struct fs_file_t f;
	uint8_t buf[128];
	char path[SHELL_PATH_MAX];

	if (shell_redir_begin(sh, &argc, argv) < 0) {
		shell_error(sh, "cat: cannot open redirect target");
		return -1;
	}
	if (argc < 2) {
		shell_error(sh, "Usage: cat <file> [> out]");
		shell_redir_end(sh);
		return -1;
	}

	shell_path_resolve(sh, argv[1], path, sizeof(path));
	fs_file_t_init(&f);

	if (fs_open(&f, path, FS_O_READ) != 0) {
		shell_error(sh, "Cannot open: %s", path);
		shell_redir_end(sh);
		return -1;
	}

	ssize_t n;

	while ((n = fs_read(&f, buf, sizeof(buf))) > 0) {
		sh_write(sh, (char *)buf, (size_t)n);
	}
	fs_close(&f);
	shell_redir_end(sh);
	return 0;
}

static int cmd_hexdump(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: hexdump <file>");
		return -1;
	}

	struct fs_file_t f;
	uint8_t buf[16];
	char path[SHELL_PATH_MAX];

	shell_path_resolve(sh, argv[1], path, sizeof(path));
	fs_file_t_init(&f);

	if (fs_open(&f, path, FS_O_READ) != 0) {
		shell_error(sh, "Cannot open: %s", path);
		return -1;
	}

	uint32_t offset = 0;
	ssize_t n;

	while ((n = fs_read(&f, buf, sizeof(buf))) > 0) {
		shell_fprintf(sh, SHELL_NORMAL, "%08x  ", offset);
		for (ssize_t i = 0; i < n; i++) {
			shell_fprintf(sh, SHELL_NORMAL, "%02x ", buf[i]);
		}
		for (ssize_t i = n; i < 16; i++) {
			shell_fprintf(sh, SHELL_NORMAL, "   ");
		}
		shell_fprintf(sh, SHELL_NORMAL, " |");
		for (ssize_t i = 0; i < n; i++) {
			char c = (buf[i] >= 0x20 && buf[i] < 0x7f)
					 ? (char)buf[i]
					 : '.';
			shell_fprintf(sh, SHELL_NORMAL, "%c", c);
		}
		shell_print(sh, "|");
		offset += (uint32_t)n;
	}
	fs_close(&f);
	shell_print(sh, "%08x  (%u bytes)", offset, offset);
	return 0;
}

static int cmd_mkdir(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: mkdir <path>");
		return -1;
	}

	char path[SHELL_PATH_MAX];

	shell_path_resolve(sh, argv[1], path, sizeof(path));
	if (fs_mkdir(path) != 0) {
		shell_error(sh, "Failed to create: %s", path);
		return -1;
	}
	shell_print(sh, "Created: %s", path);
	return 0;
}

static int cmd_rm(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: rm <file>");
		return -1;
	}

	char path[SHELL_PATH_MAX];

	shell_path_resolve(sh, argv[1], path, sizeof(path));
	if (fs_unlink(path) != 0) {
		shell_error(sh, "Failed to remove: %s", path);
		return -1;
	}
	shell_print(sh, "Removed: %s", path);
	return 0;
}

static int cmd_df(const struct shell *sh, size_t argc, char **argv)
{
	struct fs_statvfs stat;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (fs_statvfs("/SD:/", &stat) != 0) {
		shell_error(sh, "Cannot stat filesystem");
		return -1;
	}

	uint64_t total = (uint64_t)stat.f_bsize * stat.f_blocks;
	uint64_t free_bytes = (uint64_t)stat.f_bsize * stat.f_bfree;

	shell_print(sh, "Filesystem: /SD:/");
	shell_print(sh, "  Block size : %lu", (unsigned long)stat.f_bsize);
	shell_print(sh, "  Total      : %llu bytes",
		    (unsigned long long)total);
	shell_print(sh, "  Free       : %llu bytes",
		    (unsigned long long)free_bytes);
	return 0;
}

static int cmd_write(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 3) {
		shell_error(sh, "Usage: write <file> <text>");
		return -1;
	}

	struct fs_file_t f;
	char path[SHELL_PATH_MAX];

	shell_path_resolve(sh, argv[1], path, sizeof(path));
	fs_file_t_init(&f);

	if (fs_open(&f, path, FS_O_WRITE | FS_O_CREATE) != 0) {
		shell_error(sh, "Cannot open: %s", path);
		return -1;
	}

	ssize_t bw = fs_write(&f, argv[2], strlen(argv[2]));

	fs_close(&f);

	if (bw < 0) {
		shell_error(sh, "Write failed: %d", (int)bw);
		return -1;
	}
	shell_print(sh, "Wrote %d bytes to %s", (int)bw, path);
	return 0;
}

static int cmd_cp(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	char src[SHELL_PATH_MAX];
	char dst[SHELL_PATH_MAX];

	shell_path_resolve(sh, argv[1], src, sizeof(src));
	shell_path_resolve(sh, argv[2], dst, sizeof(dst));

	struct fs_file_t in;
	struct fs_file_t out;

	fs_file_t_init(&in);
	fs_file_t_init(&out);

	if (fs_open(&in, src, FS_O_READ) != 0) {
		shell_error(sh, "cp: cannot open %s", src);
		return -1;
	}
	if (fs_open(&out, dst, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC) != 0) {
		shell_error(sh, "cp: cannot create %s", dst);
		fs_close(&in);
		return -1;
	}

	uint8_t buf[256];
	ssize_t n;
	ssize_t total = 0;
	int rc = 0;

	while ((n = fs_read(&in, buf, sizeof(buf))) > 0) {
		ssize_t w = fs_write(&out, buf, n);

		if (w < 0) {
			shell_error(sh, "cp: write failed: %d", (int)w);
			rc = -1;
			break;
		}
		total += w;
	}
	fs_close(&in);
	fs_close(&out);

	if (rc == 0) {
		shell_print(sh, "Copied %d bytes: %s -> %s", (int)total, src, dst);
	}
	return rc;
}

static int cmd_mv(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	char src[SHELL_PATH_MAX];
	char dst[SHELL_PATH_MAX];

	shell_path_resolve(sh, argv[1], src, sizeof(src));
	shell_path_resolve(sh, argv[2], dst, sizeof(dst));

	if (fs_rename(src, dst) != 0) {
		shell_error(sh, "mv: failed %s -> %s", src, dst);
		return -1;
	}
	shell_print(sh, "Moved %s -> %s", src, dst);
	return 0;
}

static int cmd_touch(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	char path[SHELL_PATH_MAX];
	struct fs_file_t f;

	shell_path_resolve(sh, argv[1], path, sizeof(path));
	fs_file_t_init(&f);

	if (fs_open(&f, path, FS_O_CREATE | FS_O_WRITE) != 0) {
		shell_error(sh, "touch: cannot create %s", path);
		return -1;
	}
	fs_close(&f);
	return 0;
}

static int cmd_echo(const struct shell *sh, size_t argc, char **argv)
{
	if (shell_redir_begin(sh, &argc, argv) < 0) {
		shell_error(sh, "echo: cannot open redirect target");
		return -1;
	}

	for (size_t i = 1; i < argc; i++) {
		sh_write(sh, argv[i], strlen(argv[i]));
		if (i + 1 < argc) {
			sh_write(sh, " ", 1);
		}
	}
	sh_print(sh, "");   /* trailing newline */
	shell_redir_end(sh);
	return 0;
}

/* ── System commands ─────────────────────────────────────────────────────── */

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "KlaussCPU Zephyr %s", KERNEL_VERSION_STRING);
	shell_print(sh, "Uptime : %llu ms",
		    (unsigned long long)k_uptime_get());
	shell_print(sh, "Clock  : %u Hz", CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC);
	return 0;
}

#ifdef CONFIG_THREAD_MONITOR
static void thread_print_cb(const struct k_thread *t, void *ctx)
{
	const struct shell *sh = ctx;
	const char *name = k_thread_name_get((k_tid_t)t);

	shell_print(sh, "  %s  prio=%d", name ? name : "(unnamed)",
		    k_thread_priority_get((k_tid_t)t));
}
#endif

static int cmd_threads(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Active threads:");
#ifdef CONFIG_THREAD_MONITOR
	k_thread_foreach(thread_print_cb, (void *)sh);
#else
	shell_print(sh, "  (enable CONFIG_THREAD_MONITOR)");
#endif
	return 0;
}

/* ── Hardware commands ───────────────────────────────────────────────────── */

static int cmd_leds(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		unsigned long val = strtoul(argv[1], NULL, 16);

		REG_LEDS = (uint32_t)val;
		shell_print(sh, "LEDs = 0x%04lx", val & 0xFFFF);
	} else {
		shell_print(sh, "LEDs = 0x%04lx",
			    (unsigned long)(REG_LEDS & 0xFFFF));
	}
	return 0;
}

static int cmd_seg(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: seg <hex32>");
		return -1;
	}
	unsigned long val = strtoul(argv[1], NULL, 16);

	REG_SEG_ALL = (uint32_t)val;
	shell_print(sh, "7-seg = 0x%08lx", val);
	return 0;
}

static int cmd_switches(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "Switches = 0x%04lx",
		    (unsigned long)(REG_SWITCHES & 0xFFFF));
	return 0;
}

static int cmd_uptime(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int64_t ms = k_uptime_get();
	int64_t sec = ms / 1000;
	int64_t min = sec / 60;
	int64_t hr = min / 60;

	shell_print(sh, "%02lld:%02lld:%02lld.%03lld",
		    (long long)hr, (long long)(min % 60),
		    (long long)(sec % 60), (long long)(ms % 1000));
	return 0;
}

/* ── Performance counters — live system profiler ─────────────────────────── */
/*
 * `perf [ms]` delta-samples the MMIO performance counters (CPU pipeline at
 * 0xF00D, cache at 0xF005) over a window (default 1000 ms) and reports CPI,
 * busy/idle split, cycle accounting, instruction mix, branch behaviour, and
 * cache hit/miss/penalty for whatever the live system did during that window —
 * i.e. the real SSH/lwIP/idle workload, not a synthetic benchmark.  Sampling is
 * non-destructive (read, sleep, read, subtract); it does not clear the counters.
 *
 * All shell_print/shell_fprintf args are 64-bit (%llu + 8-byte %s) so the
 * Zephyr cbprintf mixed-32/64-bit packing issue (see CLAUDE.md "Known issues")
 * cannot occur here.
 */

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
	s->rh      = REG64(0xF0050040u); s->rm    = REG64(0xF0050048u);
	s->wh      = REG64(0xF0050050u); s->wm    = REG64(0xF0050058u);
	s->wb      = REG64(0xF0050060u); s->stall = REG64(0xF0050068u);
}

/* print "<label>NN.NN% " using only 64-bit args (cbprintf-safe) */
static void sh_pct(const struct shell *sh, const char *label,
		   uint64_t num, uint64_t den)
{
	uint64_t p = den ? (num * 10000ull) / den : 0;   /* hundredths of % */

	shell_fprintf(sh, SHELL_NORMAL, "%s%llu.%02llu%% ", label,
		      (unsigned long long)(p / 100), (unsigned long long)(p % 100));
}

static int cmd_perf(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t ms = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 10) : 1000u;

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

	shell_print(sh, "perf window=%llums  cycles=%llu  instr=%llu",
		    (unsigned long long)ms, (unsigned long long)cyc,
		    (unsigned long long)ins);

	if (ins) {
		uint64_t cpi = (cyc * 1000ull) / ins;

		shell_print(sh, "CPI=%llu.%03llu",
			    (unsigned long long)(cpi / 1000),
			    (unsigned long long)(cpi % 1000));
	} else {
		shell_print(sh, "CPI=n/a (idle: no instructions retired)");
	}

	shell_fprintf(sh, SHELL_NORMAL, "util: busy=");
	sh_pct(sh, "", cyc - idle, cyc);
	sh_pct(sh, "idle=", idle, cyc);
	shell_print(sh, "");

	shell_fprintf(sh, SHELL_NORMAL, "cyc:  ");
	sh_pct(sh, "fetch=", DLT(fetch), cyc);
	sh_pct(sh, "exec=",  DLT(exec),  cyc);
	sh_pct(sh, "mul=",   DLT(mul),   cyc);
	sh_pct(sh, "div=",   DLT(div),   cyc);
	sh_pct(sh, "int=",   DLT(intc),  cyc);
	sh_pct(sh, "idle=",  idle,       cyc);
	shell_print(sh, "");

	shell_fprintf(sh, SHELL_NORMAL, "mix:  ");
	sh_pct(sh, "ALU=",  DLT(alu),    ins);
	sh_pct(sh, "LD=",   DLT(load),   ins);
	sh_pct(sh, "ST=",   DLT(store),  ins);
	sh_pct(sh, "BR=",   DLT(branch), ins);
	sh_pct(sh, "JMP=",  DLT(jump),   ins);
	sh_pct(sh, "CALL=", DLT(call),   ins);
	sh_pct(sh, "IND=",  DLT(ind),    ins);
	sh_pct(sh, "OTH=",  DLT(other),  ins);
	shell_print(sh, "");

	uint64_t br = DLT(branch), tk = DLT(taken);

	shell_fprintf(sh, SHELL_NORMAL, "branch: n=%llu taken=%llu rate=",
		      (unsigned long long)br, (unsigned long long)tk);
	sh_pct(sh, "", tk, br);
	shell_print(sh, "");

	shell_fprintf(sh, SHELL_NORMAL, "cache: acc=%llu miss=",
		      (unsigned long long)acc);
	sh_pct(sh, "", misses, acc);
	shell_fprintf(sh, SHELL_NORMAL, "wb=%llu stall=%llu",
		      (unsigned long long)DLT(wb), (unsigned long long)DLT(stall));
	if (misses) {
		uint64_t pen = (DLT(stall) * 100ull) / misses;

		shell_fprintf(sh, SHELL_NORMAL, " avgpen=%llu.%02lluc",
			      (unsigned long long)(pen / 100),
			      (unsigned long long)(pen % 100));
	}
	shell_print(sh, "");

	uint64_t mo = DLT(mul_ops), dop = DLT(div_ops), io = DLT(int_ops);

	if (mo || dop || io) {
		shell_fprintf(sh, SHELL_NORMAL,
			      "ops: mul=%llu div=%llu irq=%llu",
			      (unsigned long long)mo, (unsigned long long)dop,
			      (unsigned long long)io);
		if (dop) {
			uint64_t lat = (DLT(div) * 100ull) / dop;

			shell_fprintf(sh, SHELL_NORMAL, " divlat=%llu.%02lluc",
				      (unsigned long long)(lat / 100),
				      (unsigned long long)(lat % 100));
		}
		shell_print(sh, "");
	}
#undef DLT
	return 0;
}

/* ── Working directory ───────────────────────────────────────────────────── */

static int cmd_cd(const struct shell *sh, size_t argc, char **argv)
{
	/* `cd` with no arg → root (home). */
	if (argc < 2) {
		shell_cwd_reset(sh);
		shell_prompt_sync(sh);
		return 0;
	}

	char path[SHELL_PATH_MAX];

	shell_path_resolve(sh, argv[1], path, sizeof(path));

	struct fs_dir_t dir;

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, path) != 0) {
		shell_error(sh, "cd: not a directory: %s", path);
		return -1;
	}
	fs_closedir(&dir);

	shell_cwd_set(sh, path);
	shell_prompt_sync(sh);
	return 0;
}

static int cmd_pwd(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "%s", shell_cwd(sh));
	return 0;
}

/* ── Tab-completion: entries of the completing session's cwd ─────────────────
 * Zephyr's shell_dynamic_get callback gets neither the shell nor the typed
 * token, so we recover the shell from the running thread (the completion runs
 * on that shell's own thread) to read its cwd, and we can only offer the cwd's
 * immediate entries (no descending into a typed "subdir/").  The candidate
 * string is used by the core immediately, so one static scratch buffer is OK. */

static const struct shell *completing_shell(void)
{
	k_tid_t cur = k_current_get();

	STRUCT_SECTION_FOREACH(shell, sh) {
		if (sh->thread == cur) {
			return sh;
		}
	}
	return NULL;
}

static void fs_path_complete(size_t idx, struct shell_static_entry *entry)
{
	static char compl_buf[SHELL_PATH_MAX];

	entry->syntax = NULL;          /* default: no (more) candidates */
	entry->handler = NULL;         /* completion hint only — see active_cmd */
	entry->help = NULL;
	entry->subcmd = NULL;

	const struct shell *sh = completing_shell();

	if (sh == NULL) {
		return;
	}

	struct fs_dir_t dir;
	struct fs_dirent de;

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, shell_cwd(sh)) != 0) {
		return;
	}

	size_t i = 0;

	while (fs_readdir(&dir, &de) == 0 && de.name[0] != '\0') {
		if (i == idx) {
			/* trailing '/' marks directories (bash-style) */
			snprintf(compl_buf, sizeof(compl_buf), "%s%s", de.name,
				 (de.type == FS_DIR_ENTRY_DIR) ? "/" : "");
			entry->syntax = compl_buf;
			break;
		}
		i++;
	}
	fs_closedir(&dir);
}

SHELL_DYNAMIC_CMD_CREATE(fs_complete, fs_path_complete);

/* ── Register commands with the Zephyr shell ─────────────────────────────── */

/* Filesystem commands — top-level (bash-style), all cwd-relative.  Path
 * arguments tab-complete against the session's cwd via &fs_complete. */
/* ls/cat allow extra optional args for an output redirect ("[>] [file]"). */
SHELL_CMD_ARG_REGISTER(ls, &fs_complete, "List directory [path] [> out]", cmd_ls, 1, 3);
SHELL_CMD_ARG_REGISTER(cat, &fs_complete, "Display file [> out]", cmd_cat, 2, 2);
SHELL_CMD_ARG_REGISTER(hexdump, &fs_complete, "Hex dump of file", cmd_hexdump, 2, 0);
SHELL_CMD_ARG_REGISTER(mkdir, NULL, "Create directory", cmd_mkdir, 2, 0);
SHELL_CMD_ARG_REGISTER(rm, &fs_complete, "Remove file or empty dir", cmd_rm, 2, 0);
SHELL_CMD_ARG_REGISTER(cp, &fs_complete, "Copy file: cp <src> <dst>", cmd_cp, 3, 0);
SHELL_CMD_ARG_REGISTER(mv, &fs_complete, "Move/rename: mv <src> <dst>", cmd_mv, 3, 0);
SHELL_CMD_ARG_REGISTER(touch, &fs_complete, "Create empty file", cmd_touch, 2, 0);
SHELL_CMD_ARG_REGISTER(echo, NULL, "Print arguments", cmd_echo, 1, 19);
SHELL_CMD_ARG_REGISTER(write, &fs_complete, "Write text to file: write <file> <text>",
		       cmd_write, 3, 0);
SHELL_CMD_ARG_REGISTER(df, NULL, "Filesystem free space", cmd_df, 1, 0);

SHELL_CMD_ARG_REGISTER(cd, &fs_complete, "Change directory [path] (default root)", cmd_cd, 1, 1);
SHELL_CMD_ARG_REGISTER(pwd, NULL, "Print working directory", cmd_pwd, 1, 0);
SHELL_CMD_ARG_REGISTER(info, NULL, "System information", cmd_info, 1, 0);
SHELL_CMD_ARG_REGISTER(threads, NULL, "List kernel threads", cmd_threads, 1, 0);
SHELL_CMD_ARG_REGISTER(leds, NULL, "Read/write LEDs [hex]", cmd_leds, 1, 1);
SHELL_CMD_ARG_REGISTER(seg, NULL, "Write 7-segment <hex>", cmd_seg, 2, 0);
SHELL_CMD_ARG_REGISTER(switches, NULL, "Read switches", cmd_switches, 1, 0);
SHELL_CMD_ARG_REGISTER(uptime, NULL, "System uptime", cmd_uptime, 1, 0);
SHELL_CMD_ARG_REGISTER(perf, NULL, "Profile live CPU/cache over [ms] window (default 1000)",
		       cmd_perf, 1, 1);
