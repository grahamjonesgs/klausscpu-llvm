/* llext_loader.h — Load and run an LLEXT extension from SD card. */

#ifndef LLEXT_LOADER_H
#define LLEXT_LOADER_H

#include <wolfssh/ssh.h>

void llext_loader_init(void);

/*
 * llext_run_from_sd() — load an ELFCLASS32 ET_REL extension from SD, resolve
 *                       its `main`, run it in a Zephyr thread, block until it
 *                       returns, and return its exit code.
 *
 * The extension's stdout (printf/puts/putchar) and stdin (getchar) are
 * redirected to the SSH session for the duration of the run.  Status/diagnostic
 * messages are sent over SSH when ssh is non-NULL, otherwise to printk.
 *
 * Returns:
 *   >= 0  : extension main() return value
 *    -1   : file open/read failed
 *    -2   : llext_load failed
 *    -3   : `main` not found in the extension
 *    -4   : out of memory / thread resource failure
 */
int llext_run_from_sd(const char *filename, WOLFSSH *ssh);

#endif /* LLEXT_LOADER_H */
