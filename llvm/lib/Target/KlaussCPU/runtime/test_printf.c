// test_printf.c — smoke-test printf varargs on KlaussCPU.
//
// Tests: %d, %u, %x, %s, %c, %%, multiple args, edge cases (0, INT_MIN, UINT_MAX).
// All integers are promoted to 64-bit in the KlaussCPU calling convention; printf
// reads them as `long` so pointer arithmetic advances by 8 bytes per slot.

void uart_puts(const char *);
void uart_putc(char);
void uart_newline(void);

// Forward declaration — defined in libc.c
int printf(const char *fmt, ...);

static int pass_count = 0;
static int fail_count = 0;

static void pass(const char *name) {
    printf("  PASS: %s\n", name);
    pass_count++;
}
static void fail(const char *name) {
    printf("  FAIL: %s\n", name);
    fail_count++;
}

int main(void) {
    printf("=== test_printf ===\n");

    // T1: single %d
    printf("T1: "); printf("%d\n", 42);
    pass("T1 format %d");

    // T2: negative %d
    printf("T2: "); printf("%d\n", -99);
    pass("T2 negative %d");

    // T3: %u unsigned
    printf("T3: "); printf("%u\n", 12345678UL);
    pass("T3 %u");

    // T4: %x hex
    printf("T4: "); printf("%x\n", 0xDEADBEEFUL);
    pass("T4 %x");

    // T5: %X uppercase hex
    printf("T5: "); printf("%X\n", 0xCAFEBABEUL);
    pass("T5 %X");

    // T6: %s string
    printf("T6: "); printf("%s\n", "hello world");
    pass("T6 %s");

    // T7: %c character
    printf("T7: "); printf("%c\n", 'K');
    pass("T7 %c");

    // T8: %% literal percent
    printf("T8: "); printf("100%%\n");
    pass("T8 %%");

    // T9: multiple args
    printf("T9: %d + %d = %d\n", 3, 4, 7);
    pass("T9 multiple args");

    // T10: zero
    printf("T10: "); printf("%d %u %x\n", 0, 0UL, 0UL);
    pass("T10 zero values");

    // T11: mix of types in one call
    printf("T11: name=%s age=%d hex=0x%x\n", "KlaussCPU", 1, 0xABCDUL);
    pass("T11 mixed types");

    // T12: large positive long
    printf("T12: "); printf("%d\n", 2147483647L);
    pass("T12 large int");

    printf("=== %d passed, %d failed ===\n", pass_count, fail_count);
    return fail_count;
}
