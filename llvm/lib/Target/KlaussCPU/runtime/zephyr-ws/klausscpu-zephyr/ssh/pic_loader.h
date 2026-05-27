/* pic_loader.h — Load and run a PIC ELF from SD card (Zephyr port). */

#ifndef PIC_LOADER_H
#define PIC_LOADER_H

#include <wolfssh/ssh.h>

void pic_loader_init(void);

/*
 * pic_run_from_sd() — load filename from SD, run it in a Zephyr thread,
 *                     block until it exits, and return its exit code.
 *
 * If ssh is non-NULL, the program's stdout is mirrored to the SSH session
 * and status messages are sent over SSH.  If ssh is NULL, status goes to
 * printk and the program gets NULL as its mirror callback (UART only).
 *
 * Returns:
 *   >= 0  : program exit code
 *    -1   : file open or read failed
 *    -2   : ELF/flat load failed
 *    -3   : thread resource allocation failed
 */
int pic_run_from_sd(const char *filename, WOLFSSH *ssh);

#endif /* PIC_LOADER_H */
