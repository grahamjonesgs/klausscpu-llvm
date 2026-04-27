/*
 * hello.c — Step 22 smoke test for KlaussCPU
 *
 * Uses hardware UART instructions via uart_stubs.c.
 * No memory-mapped addresses needed — the CPU handles UART internally.
 */

void uart_puts(const char *s);
void uart_newline(void);
void leds(unsigned long long val);
void seg7(unsigned long long val);

int main(int argc, char **argv) {
    leds(0x0001);               // checkpoint 1: entered main
    seg7(0x0001);

    uart_puts("Hello, KlaussCPU!");
    leds(0x0003);               // checkpoint 2: puts returned
    seg7(0x0003);

    uart_newline();
    leds(0x0007);               // checkpoint 3: newline returned
    seg7(0x0007);

    return 0;
}
