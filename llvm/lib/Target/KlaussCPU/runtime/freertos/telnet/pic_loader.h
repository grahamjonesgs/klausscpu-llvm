/*
 * pic_loader.h — load and run a PIC ELF from SD card, mirroring stdout
 *                to an active telnet socket.
 */

#ifndef PIC_LOADER_H
#define PIC_LOADER_H

/*
 * pic_loader_init() — create the run-once mutex.  Call before vTaskStartScheduler().
 */
void pic_loader_init(void);

/*
 * pic_run_from_sd() — load filename from SD, run it in a FreeRTOS task, block
 *                     until it exits, and return its exit code.
 *
 * While running, the program's printf output is sent to sock in addition to
 * the physical UART.  \n is expanded to \r\n for telnet compatibility.
 *
 * Returns:
 *   >= 0  : program exit code
 *    -1   : SD mount or file-open failed
 *    -2   : ELF/flat load failed
 *    -3   : FreeRTOS resource allocation failed
 */
int pic_run_from_sd(const char *filename, int sock);

#endif /* PIC_LOADER_H */
