// libc.c — minimal runtime library for KlaussCPU C programs.
//
// Provides I/O adapters, string functions, and a simple heap allocator.
// All functions assume the KlaussCPU hardware memory model:
//   - Physical memory is little-endian: byte at address X in bits[8*(X mod 4)+7:8*(X mod 4)].
//   - LDIDX8/STIDX8 use LE-lane mapping, consistent with physical memory.
//   - int = 32-bit; long, long long, void* are all 64-bit.
//     The hardware heap-header word at address 0 is 8 bytes wide, so anything
//     reading or writing it must use a 64-bit-pointer cast.

typedef unsigned long long uint64_t;
typedef long long           int64_t;

// Forward declarations from uart_stubs.c / io_stubs.c
void      uart_putc(char c);
void      uart_puts(const char *s);
void      uart_newline(void);
uint64_t  uart_getc_blocking(void);
void      uart_tx_hex(uint64_t val);

// ── I/O adapters (old rcc API names) ────────────────────────────────────────

void putchar(int c)      { uart_putc((char)c); }
void print_str(char *s)  { uart_puts(s); }
void newline(void)       { uart_newline(); }
int  getchar(void)       { return (int)uart_getc_blocking(); }

// Print signed decimal integer.
void print_int(int64_t n) {
    char buf[24];
    int i = 0;
    if (n == 0) { uart_putc('0'); return; }
    if (n < 0)  { uart_putc('-'); n = -n; }
    while (n > 0) {
        buf[i++] = (char)('0' + (int)(n % 10));
        n /= 10;
    }
    int j;
    for (j = i - 1; j >= 0; j--)
        uart_putc(buf[j]);
}

// Print "0x" followed by 16 hex digits (uses TXR hardware instruction).
void print_hex_prefix(int64_t val) {
    uart_putc('0');
    uart_putc('x');
    uart_tx_hex((uint64_t)val);
}

// ── String / memory functions ────────────────────────────────────────────────
// These use MEMGET8/STIDX8 which are self-consistent for char arrays:
// any byte written via STIDX8 at address A is read back correctly by MEMGET8
// at the same address A (both use the same scrambled physical slot).

// ── printf ───────────────────────────────────────────────────────────────────
// Minimal printf supporting: %d %i %u %x %X %c %s %ld %li %lu %lx %lX %%
//
// All integer varargs are promoted to 64-bit by the KlaussCPU calling convention,
// so we read them as `long` (= int64_t, 8 bytes) regardless of the format width.
// For %c and %s the vararg is also 8 bytes (pointer or char padded to 64 bits).

#include <stdarg.h>

// Translate \n → \r\n for serial terminals; pass all other chars unchanged.
static void _putc_crlf(char c) {
    if (c == '\n') uart_putc('\r');
    uart_putc(c);
}

static void _puthex(uint64_t v, int upper) {
    char buf[17];
    int i = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v) {
        int d = (int)(v & 0xF);
        buf[i++] = d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10;
        v >>= 4;
    }
    int j;
    for (j = i - 1; j >= 0; j--)
        uart_putc(buf[j]);
}

static void _putulong(uint64_t v) {
    char buf[22];
    int i = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v) {
        buf[i++] = '0' + (int)(v % 10);
        v /= 10;
    }
    int j;
    for (j = i - 1; j >= 0; j--)
        uart_putc(buf[j]);
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int count = 0;
    while (*fmt) {
        if (*fmt != '%') {
            _putc_crlf(*fmt++);
            count++;
            continue;
        }
        fmt++; // skip '%'
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }
        (void)is_long; // all ints promoted to 64-bit anyway
        switch (*fmt) {
        case 'd': case 'i': {
            long raw = va_arg(ap, long);
            // %d reads a C int (32-bit); the calling convention zero-extends it to
            // 64-bit (AExt), so we must sign-extend back from the lower 32 bits.
            // %ld uses the full 64-bit long value as-is.
            int64_t v = is_long ? (int64_t)raw : (int64_t)(int)raw;
            if (v < 0) { uart_putc('-'); v = -v; }
            _putulong((uint64_t)v);
            break;
        }
        case 'u': {
            long raw = va_arg(ap, long);
            // %u is an unsigned int (32-bit); mask upper bits for correctness.
            uint64_t v = is_long ? (uint64_t)raw : (uint64_t)(unsigned int)raw;
            _putulong(v);
            break;
        }
        case 'x': {
            long raw = va_arg(ap, long);
            uint64_t v = is_long ? (uint64_t)raw : (uint64_t)(unsigned int)raw;
            _puthex(v, 0);
            break;
        }
        case 'X': {
            long raw = va_arg(ap, long);
            uint64_t v = is_long ? (uint64_t)raw : (uint64_t)(unsigned int)raw;
            _puthex(v, 1);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, long);
            _putc_crlf(c);
            break;
        }
        case 's': {
            char *s = (char *)va_arg(ap, long);
            while (*s) _putc_crlf(*s++);
            break;
        }
        case '%':
            uart_putc('%');
            break;
        default:
            uart_putc('%');
            _putc_crlf(*fmt);
            break;
        }
        fmt++;
        count++;
    }
    va_end(ap);
    return count;
}

int strlen(char *s) {
    int n = 0;
    while (s[n] != '\0') n++;
    return n;
}

int strcmp(char *a, char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dst, char *src) {
    char *d = dst;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

void *memset(void *p, int val, int n) {
    char *cp = (char *)p;
    int i;
    for (i = 0; i < n; i++)
        cp[i] = (char)val;
    return p;
}

void *memcpy(void *dst, void *src, int n) {
    char *d = (char *)dst;
    char *s = (char *)src;
    int i;
    for (i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

// ── Heap allocator ───────────────────────────────────────────────────────────
// Block layout — 24-byte (3 × 8-byte word) header per allocation:
//   word 0: user data size in bytes (always a multiple of 8)
//   word 1: flags — 0 = in-use, 1 = free
//   word 2: reserved (padding)
//   [user data follows immediately]
//
// The heap starts at _end (first byte after .bss) and grows upward.
// heap_start is written to address 0x0000 (the hardware heap-header area)
// as a 64-bit value, so test programs read it with
//   long long *mem = (long long*)0; hs = mem[0];
//
// g_heap_base was removed: the compiler eliminated it and replaced the scan
// start with literal 0, corrupting the code section.  We now use the linker
// symbol _end directly — the compiler cannot constant-fold a linker symbol.

extern char _end[];   // first byte past .bss, from linker script

#define HDR_SZ  24    // header size in bytes

// g_heap_top is non-static so crt0.c can initialise it before main().
// The compiler cannot constant-fold it (cross-TU write from crt0) so it
// must reload it at the start of every malloc call.
char *g_heap_top = 0;

static uint64_t align8(uint64_t n) {
    return (n + 7) & ~(uint64_t)7;
}

void *malloc(uint64_t size) {
    size = align8(size);

    // First-fit: search existing free blocks, always starting from _end.
    // Using _end directly (a linker symbol) prevents the compiler from
    // substituting literal 0 as the scan base.
    char *p = _end;
    while (p < g_heap_top) {
        uint64_t *hdr = (uint64_t *)p;
        if (hdr[1] == 1 && hdr[0] >= size) {   // free and big enough
            hdr[1] = 0;                         // mark in-use
            return p + HDR_SZ;
        }
        p += HDR_SZ + hdr[0];
    }

    // Extend heap.
    uint64_t *hdr = (uint64_t *)g_heap_top;
    hdr[0] = size;
    hdr[1] = 0;    // in-use
    hdr[2] = 0;    // reserved
    char *result = g_heap_top + HDR_SZ;
    g_heap_top  += HDR_SZ + size;
    return result;
}

void free(void *ptr) {
    if (!ptr) return;
    uint64_t *hdr = (uint64_t *)((char *)ptr - HDR_SZ);
    hdr[1] = 1;   // mark free

    // Coalesce with any immediately following free blocks.
    char *next = (char *)ptr + hdr[0];
    while (next < g_heap_top) {
        uint64_t *nhdr = (uint64_t *)next;
        if (nhdr[1] != 1) break;               // next block in-use
        hdr[0] += HDR_SZ + nhdr[0];            // absorb next block
        next    = (char *)ptr + hdr[0];
    }

    // Shrink heap top if this was the last block.
    if ((char *)ptr + hdr[0] == g_heap_top)
        g_heap_top = (char *)hdr;
}

void *calloc(uint64_t n, uint64_t size) {
    uint64_t total = n * size;
    void *p = malloc(total);
    if (p) memset(p, 0, (int)total);
    return p;
}

void *realloc(void *ptr, uint64_t newsize) {
    if (!ptr) return malloc(newsize);
    uint64_t *hdr    = (uint64_t *)((char *)ptr - HDR_SZ);
    uint64_t oldsize = hdr[0];
    void *newptr = malloc(newsize);
    if (!newptr) return 0;
    uint64_t copy = (oldsize < newsize) ? oldsize : newsize;
    memcpy(newptr, ptr, (int)copy);
    free(ptr);
    return newptr;
}

// Return the current heap top as a 64-bit address.  Returning `int` would
// truncate the (32-bit) address to the low 32 bits and sign-extend it to
// 64 bits when read by callers — fine for valid 128 MiB addresses, but
// confusing.  Use the unambiguous wider type.
long long heap_get_top(void) {
    return (long long)(uint64_t)g_heap_top;
}

// Return the total number of 8-byte words currently in-use by live allocations.
int heap_words_used(void) {
    int words = 0;
    char *p = _end;
    while (p < g_heap_top) {
        uint64_t *hdr = (uint64_t *)p;
        if (hdr[1] == 0)                    // in-use
            words += (int)(hdr[0] / 8);
        p += HDR_SZ + hdr[0];
    }
    return words;
}
