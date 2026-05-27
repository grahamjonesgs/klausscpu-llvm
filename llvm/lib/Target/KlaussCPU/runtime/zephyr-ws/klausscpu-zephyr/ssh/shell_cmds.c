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
