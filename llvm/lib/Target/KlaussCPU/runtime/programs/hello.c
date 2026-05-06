/*
 * hello.c — KlaussCPU hello-world smoke test
 */

#include "../mmio.h"

void uart_puts(const char *s);
void uart_newline(void);

int main(int argc, char **argv) {
    leds(0x0001);
    seg7(0x0001);

    uart_puts("Hello, KlaussCPU! 1");
    uart_newline();

    uart_puts("Hello, KlaussCPU! 2");
    uart_newline();

    leds(0x0003);
    seg7(0x0003);

    return 0;
}
