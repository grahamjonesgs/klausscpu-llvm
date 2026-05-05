// compat.c — thin wrappers mapping the old hand-written libc API to the
// picolibc + uart_stubs layer.
//
// Uses uart_puts/uart_putc directly (no stdio) so that this file compiles
// without a working stdout FILE pointer, which picolibc in freestanding mode
// does not provide.

#include <stdint.h>

// Provided by uart_stubs.c
void uart_putc(char c);
void uart_puts(const char *s);

// ── Old print API ─────────────────────────────────────────────────────────────

void print_str(const char *s) { uart_puts(s); }
void newline(void)            { uart_putc('\n'); }

void print_int(long long n) {
    if (n < 0) { uart_putc('-'); n = -n; }
    if (n >= 10) print_int(n / 10);
    uart_putc('0' + (int)(n % 10));
}

void print_hex_prefix(long long val) {
    unsigned long long v = (unsigned long long)val;
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        int nibble = (v >> i) & 0xF;
        uart_putc(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
}

// ── Heap inspection stubs ─────────────────────────────────────────────────────
// picolibc's malloc is opaque; return 0 to keep old test programs compiling.
long long heap_get_top(void)  { return 0; }
int heap_words_used(void)     { return 0; }
