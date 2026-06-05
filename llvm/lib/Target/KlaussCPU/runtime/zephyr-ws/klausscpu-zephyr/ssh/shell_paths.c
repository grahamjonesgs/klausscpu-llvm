/* shell_paths.c — per-shell working directory + path normalisation.
 * See shell_paths.h.  Shared by shell_cmds.c (cd/pwd + fs commands) and the
 * SSH transport (cwd reset on connect). */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/fs/fs.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "shell_paths.h"

#define FS_ROOT   "/SD:"        /* mount point, no trailing slash       */
#define FS_ROOTSL "/SD:/"       /* root as a directory path             */
#define N_SLOTS   4             /* serial console + up to 3 SSH slots   */
#define MAX_COMPS 32

K_MUTEX_DEFINE(cwd_mtx);

struct cwd_slot {
	const struct shell *sh;
	char cwd[SHELL_PATH_MAX];
	struct fs_file_t rf;       /* redirect target file               */
	bool redir;                /* output currently redirected to rf  */
};

static struct cwd_slot slots[N_SLOTS];

/* Return (allocating + defaulting on first use) the per-shell slot for `sh`.
 * NULL only if the table is full (shouldn't happen: N_SLOTS covers all shells). */
static struct cwd_slot *slot_for(const struct shell *sh)
{
	struct cwd_slot *free_slot = NULL;

	k_mutex_lock(&cwd_mtx, K_FOREVER);
	for (int i = 0; i < N_SLOTS; i++) {
		if (slots[i].sh == sh) {
			k_mutex_unlock(&cwd_mtx);
			return &slots[i];
		}
		if (free_slot == NULL && slots[i].sh == NULL) {
			free_slot = &slots[i];
		}
	}
	if (free_slot != NULL) {
		free_slot->sh = sh;
		strcpy(free_slot->cwd, FS_ROOTSL);
		free_slot->redir = false;
		k_mutex_unlock(&cwd_mtx);
		return free_slot;
	}
	k_mutex_unlock(&cwd_mtx);
	return NULL;
}

static char *cwd_buf(const struct shell *sh)
{
	struct cwd_slot *s = slot_for(sh);

	return s ? s->cwd : NULL;
}

const char *shell_cwd(const struct shell *sh)
{
	char *b = cwd_buf(sh);

	return b ? b : FS_ROOTSL;
}

void shell_cwd_set(const struct shell *sh, const char *abs_path)
{
	char *b = cwd_buf(sh);

	if (b != NULL) {
		k_mutex_lock(&cwd_mtx, K_FOREVER);
		strncpy(b, abs_path, SHELL_PATH_MAX - 1);
		b[SHELL_PATH_MAX - 1] = '\0';
		k_mutex_unlock(&cwd_mtx);
	}
}

void shell_cwd_reset(const struct shell *sh)
{
	shell_cwd_set(sh, FS_ROOTSL);
}

/* Collapse "." / ".." / empty + duplicate slashes in an absolute path that
 * begins with "/SD:".  Output is "/SD:/" (root) or "/SD:/a/b" (no trailing). */
static void normalize(const char *raw, char *out, size_t out_sz)
{
	char tmp[SHELL_PATH_MAX];

	strncpy(tmp, raw, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';

	char *comps[MAX_COMPS];
	int depth = 0;
	char *p = tmp + strlen(FS_ROOT);   /* skip "/SD:" prefix */

	while (*p != '\0') {
		while (*p == '/') {
			p++;
		}
		if (*p == '\0') {
			break;
		}

		char *tok = p;

		while (*p != '\0' && *p != '/') {
			p++;
		}
		if (*p == '/') {
			*p++ = '\0';
		}

		if (strcmp(tok, ".") == 0) {
			continue;
		}
		if (strcmp(tok, "..") == 0) {
			if (depth > 0) {
				depth--;
			}
			continue;
		}
		if (depth < MAX_COMPS) {
			comps[depth++] = tok;
		}
	}

	size_t n = 0;

	n += snprintf(out + n, out_sz - n, "%s", FS_ROOT);
	if (depth == 0) {
		snprintf(out + n, out_sz - n, "/");   /* "/SD:/" */
		return;
	}
	for (int i = 0; i < depth && n < out_sz; i++) {
		n += snprintf(out + n, out_sz - n, "/%s", comps[i]);
	}
}

void shell_path_resolve(const struct shell *sh, const char *in,
			char *out, size_t out_sz)
{
	const char *cwd = shell_cwd(sh);
	char raw[SHELL_PATH_MAX];

	if (in == NULL || in[0] == '\0') {
		snprintf(raw, sizeof(raw), "%s", cwd);
	} else if (strncmp(in, FS_ROOT, strlen(FS_ROOT)) == 0) {
		snprintf(raw, sizeof(raw), "%s", in);          /* already /SD:... */
	} else if (in[0] == '/') {
		snprintf(raw, sizeof(raw), "%s%s", FS_ROOT, in); /* "/x" → /SD:/x */
	} else {
		snprintf(raw, sizeof(raw), "%s/%s", cwd, in);  /* relative to cwd */
	}

	normalize(raw, out, out_sz);
}

void shell_prompt_sync(const struct shell *sh)
{
#if defined(CONFIG_SHELL_PROMPT_CHANGE) && CONFIG_SHELL_PROMPT_CHANGE
	const char *cwd = shell_cwd(sh);
	const char *suffix = " $ ";
	char p[CONFIG_SHELL_PROMPT_BUFF_SIZE];
	size_t cl = strlen(cwd);
	size_t avail = sizeof(p) - 1 - strlen(suffix);

	if (cl <= avail) {
		snprintf(p, sizeof(p), "%s%s", cwd, suffix);
	} else {
		/* Keep the tail; '<' marks that the head was clipped. */
		snprintf(p, sizeof(p), "<%s%s", cwd + (cl - (avail - 1)), suffix);
	}
	shell_prompt_change(sh, p);
#else
	ARG_UNUSED(sh);
#endif
}

/* ── Output redirection ──────────────────────────────────────────────────── */

int shell_redir_begin(const struct shell *sh, size_t *argc, char **argv)
{
	int k = -1;
	bool append = false;
	const char *fname = NULL;

	for (size_t i = 1; i < *argc; i++) {
		if (argv[i][0] != '>') {
			continue;
		}
		append = (argv[i][1] == '>');

		const char *rest = argv[i] + (append ? 2 : 1);

		if (*rest != '\0') {
			fname = rest;              /* ">file" / ">>file" */
		} else if (i + 1 < *argc) {
			fname = argv[i + 1];       /* "> file" / ">> file" */
		} else {
			return -2;                 /* '>' with no filename */
		}
		k = (int)i;
		break;
	}

	if (k < 0) {
		return 0;                          /* no redirect */
	}

	struct cwd_slot *s = slot_for(sh);

	if (s == NULL) {
		return -3;
	}

	char path[SHELL_PATH_MAX];

	shell_path_resolve(sh, fname, path, sizeof(path));
	fs_file_t_init(&s->rf);

	fs_mode_t mode = FS_O_WRITE | FS_O_CREATE |
			 (append ? FS_O_APPEND : FS_O_TRUNC);

	if (fs_open(&s->rf, path, mode) != 0) {
		return -1;
	}

	s->redir = true;
	*argc = (size_t)k;                         /* strip ">"… from argv */
	return 1;
}

void shell_redir_end(const struct shell *sh)
{
	struct cwd_slot *s = slot_for(sh);

	if (s != NULL && s->redir) {
		fs_close(&s->rf);
		s->redir = false;
	}
}

void sh_print(const struct shell *sh, const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);

	va_end(ap);
	if (n < 0) {
		return;
	}

	struct cwd_slot *s = slot_for(sh);

	if (s != NULL && s->redir) {
		fs_write(&s->rf, buf, strnlen(buf, sizeof(buf)));
		fs_write(&s->rf, "\n", 1);
	} else {
		shell_print(sh, "%s", buf);
	}
}

void sh_write(const struct shell *sh, const char *data, size_t len)
{
	struct cwd_slot *s = slot_for(sh);

	if (s != NULL && s->redir) {
		fs_write(&s->rf, data, len);
	} else {
		shell_fprintf(sh, SHELL_NORMAL, "%.*s", (int)len, data);
	}
}
