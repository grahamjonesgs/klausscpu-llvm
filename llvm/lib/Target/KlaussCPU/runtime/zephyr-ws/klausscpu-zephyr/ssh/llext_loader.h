/* llext_loader.h — Load and run an LLEXT extension from SD card. */

#ifndef LLEXT_LOADER_H
#define LLEXT_LOADER_H

#include <zephyr/shell/shell.h>

void llext_loader_init(void);

/*
 * llext_run_from_sd() — load an ELFCLASS32 ET_REL extension from SD, resolve
 *                       its `main`, run it on the calling thread, block until it
 *                       returns, and return its exit code.
 *
 * The extension's stdout (printf/puts/putchar) and stdin (getchar) are
 * redirected to `sh` for the duration of the run.  For an SSH shell, stdout
 * goes to the wolfSSH session and stdin is drained from that session's RX ring
 * (ssh_shell_getc); for the serial console (or any non-SSH shell) stdout falls
 * through to the UART and stdin returns EOF.
 *
 * Returns:
 *   >= 0  : extension main() return value
 *    -1   : file open/read failed
 *    -2   : llext_load failed
 *    -3   : `main` not found in the extension
 *    -4   : out of memory / thread resource failure
 */
int llext_run_from_sd(const char *filename, const struct shell *sh);

#endif /* LLEXT_LOADER_H */
