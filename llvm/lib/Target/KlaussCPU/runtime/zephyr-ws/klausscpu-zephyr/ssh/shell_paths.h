/* shell_paths.h — per-shell working directory + path resolution.
 *
 * The Zephyr/FatFs VFS has no per-session cwd, so we keep one per shell
 * instance (serial console + each SSH slot get their own).  Filesystem
 * commands resolve their path argument through shell_path_resolve() so that
 * relative paths, ".", ".." and a bare "/" (= the SD root) all work like a
 * normal shell.  The on-disk mount is "/SD:"; users can type "/SD:/x",
 * "/x" (→ /SD:/x), or "x"/"../x" (relative to cwd).
 */

#ifndef SHELL_PATHS_H
#define SHELL_PATHS_H

#include <stddef.h>
#include <zephyr/shell/shell.h>

#define SHELL_PATH_MAX 192

/* Current working directory of `sh` (always absolute, e.g. "/SD:/" or
 * "/SD:/docs").  Never NULL. */
const char *shell_cwd(const struct shell *sh);

/* Set `sh`'s cwd to an already-resolved absolute path (caller validates it is
 * a directory). */
void shell_cwd_set(const struct shell *sh, const char *abs_path);

/* Reset `sh`'s cwd to the root "/SD:/" (call when a session starts). */
void shell_cwd_reset(const struct shell *sh);

/* Resolve `in` (relative or absolute, with ./..) against `sh`'s cwd into a
 * normalised absolute path in `out`.  `in` NULL/empty resolves to the cwd. */
void shell_path_resolve(const struct shell *sh, const char *in,
			char *out, size_t out_sz);

/* Update `sh`'s on-screen prompt to "<cwd> $ " (truncated if it would not fit
 * CONFIG_SHELL_PROMPT_BUFF_SIZE).  No-op if CONFIG_SHELL_PROMPT_CHANGE is off. */
void shell_prompt_sync(const struct shell *sh);

/* ── Output redirection (cmd > file / cmd >> file) ───────────────────────────
 * The shell tokenises ">"/">>" into argv, so a command opts in by calling
 * shell_redir_begin() first (which parses + strips the redirect from argv and
 * opens the target) and shell_redir_end() before returning.  In between, the
 * command must emit output via sh_print()/sh_write() rather than shell_print(),
 * so it can be sent to the file instead of the terminal.  Errors should still
 * use shell_error() (they always go to the terminal, like a real shell). */

/* Parse a trailing ">"/">>" + filename out of argv (forms: "> f", ">f",
 * ">> f", ">>f"), open it for the session, and reduce *argc to drop the
 * redirect tokens.  Returns 1 if a redirect was armed, 0 if none, <0 on error. */
int shell_redir_begin(const struct shell *sh, size_t *argc, char **argv);

/* Close + disarm any redirect armed for sh (idempotent). */
void shell_redir_end(const struct shell *sh);

/* Command output sinks: the redirect file when armed, else the shell.
 * sh_print() appends a newline (like shell_print); sh_write() is raw bytes. */
void sh_print(const struct shell *sh, const char *fmt, ...);
void sh_write(const struct shell *sh, const char *data, size_t len);

#endif /* SHELL_PATHS_H */
