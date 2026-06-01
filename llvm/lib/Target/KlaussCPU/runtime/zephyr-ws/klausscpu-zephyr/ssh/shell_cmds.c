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
#include <stdlib.h>

/* MMIO shortcuts */
#define REG(a)        (*(volatile uint32_t *)(unsigned long)(a))
#define REG64(a)      (*(volatile uint64_t *)(unsigned long)(a))
#define REG_LEDS      REG(0xF0040000u)
#define REG_SWITCHES  REG(0xF0040008u)
#define REG_SEG_ALL   REG(0xF0030010u)

/* ── Filesystem commands ─────────────────────────────────────────────────── */

static int cmd_ls(const struct shell *sh, size_t argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1] : "/SD:/";
	struct fs_dir_t dir;
	struct fs_dirent entry;

	fs_dir_t_init(&dir);

	if (fs_opendir(&dir, path) != 0) {
		shell_error(sh, "Cannot open directory: %s", path);
		return -1;
	}

	shell_print(sh, "Directory: %s", path);

	while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
		if (entry.type == FS_DIR_ENTRY_DIR) {
			shell_print(sh, "  [DIR]  %s", entry.name);
		} else {
			shell_print(sh, "  %6u  %s", (unsigned)entry.size,
				    entry.name);
		}
	}
	fs_closedir(&dir);
	return 0;
}

static int cmd_cat(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: cat <file>");
		return -1;
	}

	struct fs_file_t f;
	uint8_t buf[128];

	fs_file_t_init(&f);

	if (fs_open(&f, argv[1], FS_O_READ) != 0) {
		shell_error(sh, "Cannot open: %s", argv[1]);
		return -1;
	}

	ssize_t n;

	while ((n = fs_read(&f, buf, sizeof(buf) - 1)) > 0) {
		buf[n] = '\0';
		shell_fprintf(sh, SHELL_NORMAL, "%s", (char *)buf);
	}
	fs_close(&f);
	shell_print(sh, "");
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

	fs_file_t_init(&f);

	if (fs_open(&f, argv[1], FS_O_READ) != 0) {
		shell_error(sh, "Cannot open: %s", argv[1]);
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
	if (fs_mkdir(argv[1]) != 0) {
		shell_error(sh, "Failed to create: %s", argv[1]);
		return -1;
	}
	shell_print(sh, "Created: %s", argv[1]);
	return 0;
}

static int cmd_rm(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: rm <file>");
		return -1;
	}
	if (fs_unlink(argv[1]) != 0) {
		shell_error(sh, "Failed to remove: %s", argv[1]);
		return -1;
	}
	shell_print(sh, "Removed: %s", argv[1]);
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

/* ── Register commands with the Zephyr shell ─────────────────────────────── */

SHELL_STATIC_SUBCMD_SET_CREATE(
	fs_cmds,
	SHELL_CMD_ARG(ls, NULL, "List directory [path]", cmd_ls, 1, 1),
	SHELL_CMD_ARG(cat, NULL, "Display file", cmd_cat, 2, 0),
	SHELL_CMD_ARG(hexdump, NULL, "Hex dump of file", cmd_hexdump, 2, 0),
	SHELL_CMD_ARG(mkdir, NULL, "Create directory", cmd_mkdir, 2, 0),
	SHELL_CMD_ARG(rm, NULL, "Remove file", cmd_rm, 2, 0),
	SHELL_CMD_ARG(df, NULL, "Filesystem free space", cmd_df, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(fs, &fs_cmds, "Filesystem commands", NULL);

SHELL_CMD_ARG_REGISTER(leds, NULL, "Read/write LEDs [hex]", cmd_leds, 1, 1);
SHELL_CMD_ARG_REGISTER(seg, NULL, "Write 7-segment <hex>", cmd_seg, 2, 0);
SHELL_CMD_ARG_REGISTER(switches, NULL, "Read switches", cmd_switches, 1, 0);
SHELL_CMD_ARG_REGISTER(uptime, NULL, "System uptime", cmd_uptime, 1, 0);
SHELL_CMD_ARG_REGISTER(perf, NULL, "Profile live CPU/cache over [ms] window (default 1000)",
		       cmd_perf, 1, 1);
