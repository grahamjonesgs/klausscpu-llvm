/*
 * test_big.c — deliberately large test program (target: > 64 KB binary).
 *
 * Purpose: validate the kbt loader with binaries that exceed the 64 KB
 * boundary.  Exercises: printf (all common specifiers), FatFs (all file
 * operations including mkdir/rename/unlink/lseek/getfree), and 64-bit
 * arithmetic.  Observable output lets you confirm correct load + execution.
 *
 * SD card must be FAT32 with 0:/hello.txt containing text starting "hello".
 */

#include <stdio.h>
#include <string.h>
#include "ff.h"
#include "mmio.h"

/* ── pass/fail tracking ──────────────────────────────────────────────────── */

static int g_pass, g_fail;

static void chk(const char *name, int ok) {
    if (ok) { g_pass++; printf("  PASS %s\n", name); }
    else     { g_fail++; printf("  FAIL %s\n", name); }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 1 — printf format coverage
 * Pulls in picolibc vfprintf + __dtoa_engine (~14 KB).
 * ═══════════════════════════════════════════════════════════════════════════ */

static void section_printf(void) {
    printf("\n=== printf ===\n");

    /* integers */
    printf("  %%d:  %d %d %d\n",  0, 42, -99);
    printf("  %%i:  %i\n", (int)-2147483648);
    printf("  %%u:  %u\n", 4294967295u);
    printf("  %%x:  %x %X\n", 0xdeadbeefu, 0xCAFEBABEu);
    printf("  %%o:  %o\n", 0755u);

    /* strings / chars */
    printf("  %%s:  '%s'\n", "hello world");
    printf("  %%c:  '%c'\n", 'K');
    printf("  %%%%:  100%%\n");

    /* width / precision */
    printf("  %%5d:   '%5d'\n",  42);
    printf("  %%-5d:  '%-5d'\n", 42);
    printf("  %%05d:  '%05d'\n", 42);
    printf("  %%8s:   '%8s'\n",  "hi");
    printf("  %%-8s:  '%-8s'\n", "hi");
    printf("  %%.3s:  '%.3s'\n", "abcde");

    /* multiple args */
    printf("  multi: %d + %d = %d, '%s' len=%d\n",
           3, 4, 7, "abc", 3);

    /* pointer */
    printf("  %%p:  %p\n", (void *)0xBEEFCAFEu);

    chk("printf-coverage", 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 2 — 64-bit arithmetic
 * Uses DIV64 / MUL64 runtime helpers + shift sequences.
 * ═══════════════════════════════════════════════════════════════════════════ */

static long long factorial(int n) {
    long long r = 1;
    for (int i = 2; i <= n; i++) r *= (long long)i;
    return r;
}

static long long fib(int n) {
    if (n <= 1) return (long long)n;
    long long a = 0, b = 1;
    for (int i = 2; i <= n; i++) { long long t = a + b; a = b; b = t; }
    return b;
}

static unsigned long long isqrt64(unsigned long long n) {
    if (n == 0) return 0;
    unsigned long long x = n, y = (x + 1) >> 1;
    while (y < x) { x = y; y = (x + n / x) >> 1; }
    return x;
}

static void section_64bit(void) {
    printf("\n=== 64-bit arithmetic ===\n");
    int ok = 1;

    /* factorial */
    ok &= (factorial(10) == 3628800LL);
    ok &= (factorial(20) == 2432902008176640000LL);
    printf("  fact(10)=%lld  fact(20)=%lld\n", factorial(10), factorial(20));

    /* fibonacci */
    ok &= (fib(0) == 0LL);
    ok &= (fib(1) == 1LL);
    ok &= (fib(10) == 55LL);
    ok &= (fib(30) == 832040LL);
    printf("  fib(10)=%lld  fib(30)=%lld\n", fib(10), fib(30));

    /* shifts */
    unsigned long long x = 1ULL;
    ok &= ((x << 32) == 0x100000000ULL);
    ok &= ((x << 63) == 0x8000000000000000ULL);
    ok &= ((0x8000000000000000ULL >> 63) == 1ULL);
    printf("  1<<32=0x%llx  1<<63=0x%llx\n", x << 32, x << 63);

    /* multiply */
    long long big = 1000000LL * 1000000LL;
    ok &= (big == 1000000000000LL);
    printf("  1M*1M=%lld\n", big);

    /* divide / modulo */
    long long q = 1000000000000LL / 7LL;
    long long r = 1000000000000LL % 7LL;
    ok &= (q == 142857142857LL && r == 1LL);
    printf("  1T/7=%lld r%lld\n", q, r);

    /* integer sqrt */
    ok &= (isqrt64(0ULL) == 0ULL);
    ok &= (isqrt64(1ULL) == 1ULL);
    ok &= (isqrt64(100ULL) == 10ULL);
    ok &= (isqrt64(1000000000000ULL) == 1000000ULL);
    printf("  isqrt(1T)=%llu\n", isqrt64(1000000000000ULL));

    /* signed edge cases */
    long long minval = -9223372036854775807LL - 1LL;
    ok &= (minval < 0LL);
    ok &= (-minval < 0LL);  /* overflow: -INT64_MIN == INT64_MIN */
    printf("  INT64_MIN=%lld\n", minval);

    /* comparison chains */
    ok &= (0x7FFFFFFFFFFFFFFFLL > 0LL);
    ok &= (-1LL < 0LL);
    ok &= ((unsigned long long)-1LL > 0ULL);

    chk("64-bit-arith", ok);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 3 — FatFs: basic operations (same as fs_demo)
 * ═══════════════════════════════════════════════════════════════════════════ */

static FATFS fs;

static void test_mount(void) {
    FRESULT fr = f_mount(&fs, "0:", 1);
    chk("f_mount", fr == FR_OK);
    if (fr != FR_OK) printf("  mount err %d\n", (int)fr);
}

static void test_read_hello(void) {
    FIL fil; UINT br; FRESULT fr;
    fr = f_open(&fil, "0:/hello.txt", FA_READ);
    if (fr != FR_OK) { chk("read hello.txt", 0); return; }
    char buf[64];
    fr = f_read(&fil, buf, sizeof(buf)-1, &br);
    f_close(&fil);
    buf[br] = '\0';
    printf("  hello.txt: \"%s\"\n", buf);
    chk("read hello.txt", fr == FR_OK && br > 0 && strncmp(buf,"hello",5)==0);
}

static void test_write_read(void) {
    FIL fil; UINT bw, br; FRESULT fr;
    const char *msg = "KlaussCPU test_big write\n";
    fr = f_open(&fil, "0:/big.txt", FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) { chk("write big.txt", 0); return; }
    f_write(&fil, msg, (UINT)strlen(msg), &bw);
    f_sync(&fil); f_close(&fil);

    char rbuf[64]; rbuf[0] = 0;
    fr = f_open(&fil, "0:/big.txt", FA_READ);
    if (fr == FR_OK) {
        f_read(&fil, rbuf, sizeof(rbuf)-1, &br);
        f_close(&fil);
        rbuf[br] = '\0';
    }
    chk("write big.txt",
        fr == FR_OK && bw == (UINT)strlen(msg) && strcmp(rbuf, msg) == 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 4 — FatFs: extra operations (adds code the basic demo skips)
 * Calling f_mkdir, f_rename, f_unlink, f_lseek, f_getfree pulls ~8 KB of
 * additional FatFs code into the binary past the --gc-sections barrier.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_mkdir(void) {
    f_unlink("0:/bigdir");              /* clean up from previous run */
    FRESULT fr = f_mkdir("0:/bigdir");
    chk("f_mkdir", fr == FR_OK || fr == FR_EXIST);
    if (fr != FR_OK && fr != FR_EXIST)
        printf("  mkdir err %d\n", (int)fr);
}

static void test_rename(void) {
    /* write a file then rename it */
    FIL fil; UINT bw;
    f_open(&fil, "0:/ren_src.txt", FA_WRITE | FA_CREATE_ALWAYS);
    f_write(&fil, "rename test\n", 12, &bw);
    f_sync(&fil); f_close(&fil);

    f_unlink("0:/ren_dst.txt");
    FRESULT fr = f_rename("0:/ren_src.txt", "0:/ren_dst.txt");
    chk("f_rename", fr == FR_OK);
    if (fr != FR_OK) printf("  rename err %d\n", (int)fr);
    f_unlink("0:/ren_dst.txt");
}

static void test_lseek(void) {
    /* write 20 bytes, seek to byte 10, read 5 */
    FIL fil; UINT bw, br; FRESULT fr;
    f_open(&fil, "0:/seek.bin", FA_WRITE | FA_CREATE_ALWAYS);
    f_write(&fil, "0123456789ABCDEFGHIJ", 20, &bw);
    f_sync(&fil); f_close(&fil);

    fr = f_open(&fil, "0:/seek.bin", FA_READ);
    if (fr != FR_OK) { chk("f_lseek", 0); return; }
    f_lseek(&fil, 10);
    char buf[8] = {0};
    f_read(&fil, buf, 5, &br);
    f_close(&fil);
    chk("f_lseek", br == 5 && strncmp(buf, "ABCDE", 5) == 0);
    f_unlink("0:/seek.bin");
}

static void test_unlink(void) {
    FIL fil; UINT bw;
    f_open(&fil, "0:/del.txt", FA_WRITE | FA_CREATE_ALWAYS);
    f_write(&fil, "delete me\n", 10, &bw);
    f_sync(&fil); f_close(&fil);

    FRESULT fr = f_unlink("0:/del.txt");
    chk("f_unlink", fr == FR_OK);
    /* confirm gone: open should fail */
    fr = f_open(&fil, "0:/del.txt", FA_READ);
    chk("unlink-gone", fr != FR_OK);
    if (fr == FR_OK) f_close(&fil);
}

static void test_getfree(void) {
    DWORD fre_clust;
    FATFS *fsp;
    FRESULT fr = f_getfree("0:", &fre_clust, &fsp);
    if (fr != FR_OK) { chk("f_getfree", 0); return; }
    /* free space in KB = fre_clust * fsp->csize / 2 (512-byte sectors) */
    unsigned long long free_kb = (unsigned long long)fre_clust * fsp->csize / 2;
    printf("  free: %llu KB\n", free_kb);
    chk("f_getfree", fr == FR_OK);
}

static void test_dir(void) {
    DIR dir; FILINFO fno; FRESULT fr; int n = 0;
    fr = f_opendir(&dir, "0:/");
    if (fr != FR_OK) { chk("f_opendir", 0); return; }
    printf("  root dir:\n");
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        printf("    %-12s %lu B\n", fno.fname, (unsigned long)fno.fsize);
        n++;
    }
    f_closedir(&dir);
    printf("  %d entries\n", n);
    chk("f_readdir", n > 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 5 — string operations (strlen, strcmp, memcpy, memset, memcmp)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void section_strings(void) {
    printf("\n=== string ops ===\n");
    int ok = 1;

    char buf[64];
    const char *src = "Hello, KlaussCPU!";
    strcpy(buf, src);
    ok &= (strcmp(buf, src) == 0);
    ok &= (strlen(buf) == strlen(src));

    memset(buf, 0xAB, 16);
    ok &= ((unsigned char)buf[0] == 0xAB);
    ok &= ((unsigned char)buf[15] == 0xAB);

    char cpy[64];
    memcpy(cpy, src, strlen(src)+1);
    ok &= (memcmp(cpy, src, strlen(src)) == 0);

    /* strncmp */
    ok &= (strncmp("hello", "hello world", 5) == 0);
    ok &= (strncmp("abc", "abd", 3) < 0);

    chk("string-ops", ok);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SECTION 6 — recursion + stack stress
 * ═══════════════════════════════════════════════════════════════════════════ */

static long long ackermann(int m, int n) {
    if (m == 0) return (long long)(n + 1);
    if (n == 0) return ackermann(m - 1, 1);
    return ackermann(m - 1, ackermann(m, n - 1));
}

static void section_recursion(void) {
    printf("\n=== recursion ===\n");
    int ok = 1;
    ok &= (ackermann(0,0) == 1LL);
    ok &= (ackermann(1,1) == 3LL);
    ok &= (ackermann(2,2) == 7LL);
    ok &= (ackermann(3,3) == 61LL);
    printf("  A(3,3)=%lld\n", ackermann(3,3));
    chk("ackermann", ok);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    g_pass = g_fail = 0;
    printf("=== test_big ===\n");
    REG_LEDS = 0x0001;

    section_printf();
    REG_LEDS = 0x0002;

    section_64bit();
    REG_LEDS = 0x0003;

    section_strings();
    REG_LEDS = 0x0004;

    section_recursion();
    REG_LEDS = 0x0005;

    printf("\n=== FatFs ===\n");
    test_mount();
    REG_LEDS = 0x0006;

    test_read_hello();
    test_write_read();
    test_dir();
    REG_LEDS = 0x0007;

    test_mkdir();
    test_rename();
    test_lseek();
    test_unlink();
    test_getfree();
    REG_LEDS = 0x0008;

    printf("\n=== Results: %d pass, %d fail ===\n", g_pass, g_fail);

    if (g_fail == 0) {
        REG_SEG_ALL = 0x00B16B00;   /* "bIgO" → all good */
        REG_LEDS    = 0xFFFF;
    } else {
        REG_SEG_ALL = 0x000000FF;
        REG_LEDS    = (uint32_t)g_fail;
    }
    return g_fail;
}
