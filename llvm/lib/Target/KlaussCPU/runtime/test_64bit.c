/* test_64bit.c — 64-bit CPU test suite for KlaussCPU (LLVM backend port).
 *
 * Compile via Makefile:  make test_64bit.elf  &&  make test_64bit.bin
 *
 * On KlaussCPU with the LLVM backend, int = long = long long = 64 bits.
 * sizeof(int) = sizeof(void*) = 8.
 *
 * Note on string literals and char arrays (T11-T13):
 *   STIDX8/MEMGET8 use scrambled byte addressing; write+read via the same
 *   address is always consistent.  String literal comparisons via strcmp work
 *   because both sides use the same scrambled addressing.  strlen counts
 *   correctly because the null terminator is found at the expected position.
 */

/* Runtime provided by libc.c + uart_stubs.c */
extern void print_str(char *s);
extern void print_int(long long n);
extern void print_hex_prefix(long long val);
extern void newline(void);
extern int  strlen(char *s);
extern int  strcmp(char *a, char *b);
extern char *strcpy(char *dst, char *src);
extern void *memset(void *p, int val, int n);
extern void *memcpy(void *dst, void *src, int n);
extern void *malloc(unsigned long long size);
extern void *calloc(unsigned long long n, unsigned long long size);
extern void *realloc(void *ptr, unsigned long long newsize);
extern void  free(void *ptr);
extern int   heap_get_top(void);
extern int   heap_words_used(void);

/* ── Globals ──────────────────────────────────────────────────────────────── */

int g_pass;
int g_fail;
int g_val;

/* ── Test helpers ─────────────────────────────────────────────────────────── */

void check(char *name, int ok) {
    print_str(name);
    if (ok) {
        print_str("PASS");
        g_pass = g_pass + 1;
    } else {
        print_str("FAIL");
        g_fail = g_fail + 1;
    }
    newline();
}

void check_eq(char *name, int actual, int expected) {
    print_str(name);
    if (actual == expected) {
        print_str("PASS");
        g_pass = g_pass + 1;
    } else {
        print_str("FAIL  got=");
        print_hex_prefix(actual);
        print_str(" exp=");
        print_hex_prefix(expected);
        g_fail = g_fail + 1;
    }
    newline();
}

/* ── Recursive helpers ────────────────────────────────────────────────────── */

int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

int fib(int n) {
    if (n <= 0) return 0;
    if (n == 1) return 1;
    return fib(n - 1) + fib(n - 2);
}

/* ── Overflow-argument helpers ────────────────────────────────────────────── */

int sum5(int a, int b, int c, int d, int e) {
    return a + b + c + d + e;
}

int sum6(int a, int b, int c, int d, int e, int f) {
    return a + b + c + d + e + f;
}

/* ── Global access helpers ────────────────────────────────────────────────── */

void set_global(int v) { g_val = v; }
int  get_global(void)  { return g_val; }

/* ════════════════════════════════════════════════════════════════════════════
 * main
 * ════════════════════════════════════════════════════════════════════════════ */

int main(void) {
    int a, b, c, ok;
    char buf[32];

    g_pass = 0;
    g_fail = 0;

    print_str("=== 64-bit CPU Test Suite ===");
    newline();

    /* System info */
    {
        int *mem;
        int sz;
        int arr2[2];
        mem = (int *)0;
        print_str("heap_start=");
        print_hex_prefix(mem[0]);
        sz = (int)((char *)(&arr2[1]) - (char *)(&arr2[0]));
        print_str("  sizeof(int)=");
        print_int(sz);
        newline();
    }
    newline();

    /* T1: 64-bit constant shift (shift > 31) */
    a = 1;
    a = a << 40;
    b = a >> 20;
    check("T1  shift>31:     ", b == 1048576);

    /* T2: 64-bit multiplication */
    a = 0x100000;
    b = 0x100000;
    c = a * b;
    c = c >> 20;
    check("T2  mul64:        ", c == 1048576);

    /* T3: 64-bit addition crosses INT32_MAX */
    a  = 0x7FFFFFFF;
    b  = a + 1;
    ok = (b > 0) && (b == 0x80000000);
    a  = b + b;
    ok = ok && (a > 0) && (a > 0x7FFFFFFF);
    check("T3  add64 no ovf: ", ok);

    /* T4: Bitwise NOT */
    {
        int b0, a1, blow, bhigh;
        a     = 0;
        b0    = ~a;
        a1    = ~b0;
        a     = 0x5A5A5A5A;
        b     = ~a;
        blow  = b & 0xFFFFFFFF;
        bhigh = b >> 32;
        check_eq("T4a ~0==-1:       ", b0,    -1);
        check_eq("T4b ~~0==0:       ", a1,     0);
        check_eq("T4c ~5A low32:    ", blow,   0xA5A5A5A5);
        check_eq("T4d ~5A >>32==-1: ", bhigh, -1);
    }

    /* T5: Arithmetic right shift of negative */
    {
        int neg1, neg256, r1, r2, r3;
        neg1   = -1;
        neg256 = -256;
        r1 = neg1   >> 63;
        r2 = neg1   >> 1;
        r3 = neg256 >> 4;
        check_eq("T5a -1>>63==-1:   ", r1,  -1);
        check_eq("T5b -1>>1==-1:    ", r2,  -1);
        check_eq("T5c -256>>4==-16: ", r3, -16);
    }

    /* T6: Logical right shift of unsigned */
    {
        unsigned int ua;
        ua = -1;
        check_eq("T6a >>63==1:      ", (int)(ua >> 63), 1);
        check_eq("T6b >>1 positive: ", ((int)(ua >> 1) > 0), 1);
    }

    /* T7: Comparisons with large 64-bit values */
    {
        int large, neg;
        unsigned int ua, ub;
        large = 1;
        large = large << 40;
        neg   = -1;
        ua    = (unsigned int)(-1);
        ub    = (unsigned int)large;
        check_eq("T7a 2^40>0:       ", (large > 0), 1);
        check_eq("T7b 2^40>0x7F..:  ", (large > 0x7FFFFFFF), 1);
        check_eq("T7c -1<2^40:      ", (neg < large), 1);
        check_eq("T7d -1<0:         ", (neg < 0), 1);
        check_eq("T7e MaxU>2^40:    ", (int)(ua > ub), 1);
    }

    /* T8: Recursion — factorial */
    ok = (factorial(10) == 3628800);
    ok = ok && (factorial(12) == 479001600);
    check("T8  factorial:    ", ok);

    /* T9: Recursion — Fibonacci */
    ok = (fib(10) == 55);
    ok = ok && (fib(15) == 610);
    check("T9  fibonacci:    ", ok);

    /* T10: Stack arguments (5th and 6th args) */
    check_eq("T10a sum5(1..5):  ", sum5(1, 2, 3, 4, 5),          15);
    check_eq("T10b sum5(10..50):", sum5(10, 20, 30, 40, 50),      150);
    check_eq("T10c sum6(10..60):", sum6(10, 20, 30, 40, 50, 60),  210);

    /* T11: Char array byte access */
    buf[0] = 72;   /* 'H' */
    buf[1] = 101;  /* 'e' */
    buf[2] = 121;  /* 'y' */
    buf[3] = 33;   /* '!' */
    buf[4] = 0;
    ok = (buf[0] == 72) && (buf[1] == 101) &&
         (buf[2] == 121) && (buf[3] == 33) && (buf[4] == 0);
    ok = ok && (strlen(buf) == 4);
    check("T11 char[]:       ", ok);

    /* T12: String operations */
    strcpy(buf, "hello");
    ok  = (strlen(buf) == 5);
    ok  = ok && (strcmp(buf, "hello") == 0);
    ok  = ok && (strcmp(buf, "hellp") < 0);
    ok  = ok && (strcmp(buf, "helln") > 0);
    ok  = ok && (strcmp(buf, "hell")  > 0);
    ok  = ok && (strcmp(buf, "hellow") < 0);
    check("T12 strings:      ", ok);

    /* T13: memset / memcpy */
    {
        char src[24];
        char dst[24];
        memset(src,      0xAB, 8);
        memset(src + 8,  0x00, 8);
        memset(src + 16, 0x55, 8);
        ok  = ((src[0] & 0xFF) == 0xAB) && ((src[7]  & 0xFF) == 0xAB);
        ok  = ok && (src[8] == 0)  && (src[15] == 0);
        ok  = ok && ((src[16] & 0xFF) == 0x55) && ((src[23] & 0xFF) == 0x55);
        memcpy(dst, src, 24);
        ok  = ok && ((dst[0]  & 0xFF) == 0xAB);
        ok  = ok && (dst[8]  == 0);
        ok  = ok && ((dst[16] & 0xFF) == 0x55);
        check("T13 memset/cpy:   ", ok);
    }

    /* T14: Global variable read/write */
    set_global(0xDEADBEEF);
    ok = (get_global() == 0xDEADBEEF);
    set_global(0);
    ok = ok && (get_global() == 0);
    set_global(-42);
    ok = ok && (get_global() == -42);
    check("T14 globals:      ", ok);

    /* T15: Pointer arithmetic — sizeof(int)=8, p+1 advances 8 bytes */
    {
        int arr[4];
        int *p;
        int dist;
        arr[0] = 100; arr[1] = 200; arr[2] = 300; arr[3] = 400;
        p  = arr;
        ok = (*p == 100);   p = p + 1;
        ok = ok && (*p == 200); p = p + 1;
        ok = ok && (*p == 300); p = p + 1;
        ok = ok && (*p == 400);
        dist = (int)((char *)(&arr[3]) - (char *)(&arr[0]));
        ok = ok && (dist == 24);    /* 3 * sizeof(int) = 3 * 8 */
        check("T15 ptr arith:    ", ok);
    }

    /* T16: Heap — malloc, calloc, realloc, free, coalesce */
    {
        int *hp1, *hp2, *hp3;
        int hs, ht;
        int *mem = (int *)0;
        hs = mem[0];    /* heap_start written by malloc init */

        /* T16a: basic alloc + accounting */
        hp1    = (int *)malloc(16);
        hp1[0] = 0x1111;
        hp1[1] = 0x2222;
        ok = (hp1[0] == 0x1111) && (hp1[1] == 0x2222);
        ht = heap_get_top();
        ok = ok && ((ht - hs) == 40);       /* 24-byte header + 16-byte data */
        ok = ok && (heap_words_used() == 2);
        check("T16a heap alloc:  ", ok);

        /* T16b: calloc zero-fill */
        hp2 = (int *)calloc(3, 8);
        ok  = (hp2[0] == 0) && (hp2[1] == 0) && (hp2[2] == 0);
        check("T16b calloc:      ", ok);

        /* T16c: realloc grow, old data survives */
        hp3    = (int *)malloc(8);
        hp3[0] = 0x5A5A5A5A;
        hp3    = (int *)realloc(hp3, 24);
        check_eq("T16c-0 keep[0]:   ", hp3[0], 0x5A5A5A5A);
        hp3[1] = 0xBEEFBEEF;
        hp3[2] = 0xCAFECAFE;
        check_eq("T16c-1 new[1]:    ", hp3[1], 0xBEEFBEEF);
        check_eq("T16c-2 new[2]:    ", hp3[2], 0xCAFECAFE);

        /* T16d: free all, verify full coalescing */
        free(hp1);
        free(hp2);
        free(hp3);
        ok = (heap_words_used() == 0);
        check("T16d coalesce:    ", ok);
    }

    /* Summary */
    newline();
    print_str("Results: ");
    print_int(g_pass);
    print_str(" pass, ");
    print_int(g_fail);
    print_str(" fail");
    newline();

    return g_fail;
}
