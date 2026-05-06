/*
 * test_fatfs_printf.c — isolation test: FatFs linked but no FatFs calls,
 * just printf.  If this crashes, the mere presence of FatFs objects (ff.o
 * etc.) in the binary causes printf to fail.
 * If this passes, the crash is triggered by actual FatFs runtime operations
 * (f_mount etc.) corrupting the stdio state before printf runs.
 */

#include <stdio.h>
#include "mmio.h"

extern void uart_puts(const char *s);

int main(void) {
    uart_puts("A: before printf\n");
    REG_LEDS = 0x0001;

    printf("B: printf %d %s\n", 42, "hello");
    REG_LEDS = 0x0002;

    printf("C: neg=%d hex=0x%x\n", -7, 0xABCDu);
    REG_LEDS = 0x0003;

    uart_puts("D: after printf\n");
    REG_LEDS = 0xDEAD;
    return 0;
}
