/*
 * test_pic.c — Smoke test for PIC loadable program execution.
 *
 * This is loaded by loader.elf via the SD card.  It exercises:
 *   - PC-relative branches  (JMPREL/JMPxxREL in every if/loop)
 *   - PC-relative calls     (CALLREL for compute/helper)
 *   - PC-relative global access  (LEAPC for g_base, g_result)
 *   - picolibc printf via UART (inherited from loader)
 *
 * Returns 0 on pass, non-zero on first failure.
 */

#include <stdio.h>

/* Globals — accessed via LEAPC in PIC mode. */
static int         g_counter = 0;
static const char *g_label   = "test_pic";

static int compute(int n) {
    int acc = 0;
    for (int i = 1; i <= n; i++)
        acc += i;          /* triangular number */
    return acc;
}

static int verify(int got, int want, int id) {
    if (got != want) {
        printf("FAIL T%d: got %d want %d\n", id, got, want);
        return 1;
    }
    g_counter++;
    return 0;
}

int main(void) {
    printf("=== %s ===\n", g_label);

    int fail = 0;

    /* T1: arithmetic loop */
    fail |= verify(compute(10), 55, 1);

    /* T2: branch chain */
    int x = 7;
    if (x > 5)  x += 3;    /* x = 10 */
    if (x < 5)  x -= 1;    /* skip */
    if (x == 10) x *= 2;   /* x = 20 */
    fail |= verify(x, 20, 2);

    /* T3: pointer through global */
    const char *p = g_label;
    int len = 0;
    while (*p++) len++;
    fail |= verify(len, 8, 3);   /* strlen("test_pic") = 8 */

    /* T4: recursive descent */
    int fib = 0, a = 0, b = 1;
    for (int i = 0; i < 10; i++) {
        int tmp = a + b;
        a = b;
        b = tmp;
    }
    fib = b;   /* fib(11) = 89 */
    fail |= verify(fib, 89, 4);

    /* T5: nested function calls */
    fail |= verify(compute(compute(3)), compute(6), 5);   /* compute(6) = 21 */

    printf("pass=%d fail=%d\n", g_counter, fail);
    return fail;
}
