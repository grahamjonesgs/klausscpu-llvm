/*
 * hello.c — Step 22 smoke test for KlaussCPU
 *
 * Writes "Hello, KlaussCPU!\n" via a memory-mapped UART at 0xF0000000.
 * Adjust UART_BASE to match your hardware register map.
 */

#define UART_BASE 0xF0000000UL
#define UART_TX   (*(volatile unsigned char *)(UART_BASE + 0))

static void uart_putc(char c) {
    UART_TX = (unsigned char)c;
}

static void uart_puts(const char *s) {
    while (*s)
        uart_putc(*s++);
}

int main(int argc, char **argv) {
    uart_puts("Hello, KlaussCPU!\n");
    return 0;
}
