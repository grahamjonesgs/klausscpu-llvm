// syscalls.c — KlaussCPU libgloss: connects picolibc to the hardware.
//
// Provides:
//  - UART callbacks (_uart_put / _uart_get) used by stdio_handles.c
//  - _sbrk for heap growth (malloc)
//  - gettimeofday() so time()/gmtime()/strftime() work after clock_set_epoch()
//  - POSIX stubs (_write/_read/_exit/etc.) as fallback
//
// Build: compiled as part of every program, same flags as the program itself.
//
// NOTE: stdin/stdout/stderr and the FILE structs live in stdio_handles.c which
// does NOT include <stdio.h>, avoiding the 'FILE * const stdout' const conflict.

#include <stdio.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>
#include <stddef.h>
#include "../mmio.h"

// ── UART primitives from uart_stubs.c ────────────────────────────────────────
extern void               uart_putc(char c);
extern unsigned long long uart_getc_blocking(void);

// ── UART callbacks — non-static so stdio_handles.c can reference them ────────

int _uart_put(char c, FILE *f) {
    (void)f;
    uart_putc(c);   /* uart_putc already converts '\n' to CR+LF */
    return 0;
}

int _uart_get(FILE *f) {
    (void)f;
    return (int)(unsigned char)uart_getc_blocking();
}

// ── Wall-clock (set by SNTP / any time source) ───────────────────────────────
// clock_set_epoch(sec): call once with the Unix timestamp.  After that,
// gettimeofday() / time() / gmtime() / strftime() all work correctly.

static long long   g_epoch_sec = 0;
static long long   g_epoch_ms  = 0;   /* REG_CLOCK_MS at epoch set */

void clock_set_epoch(long long unix_sec) {
    g_epoch_sec = unix_sec;
    g_epoch_ms  = (long long)REG_CLOCK_MS;
}

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (!tv) return 0;
    if (!g_epoch_sec) { errno = ENOSYS; return -1; }
    long long elapsed_ms = (long long)REG_CLOCK_MS - g_epoch_ms;
    tv->tv_sec  = (time_t)(g_epoch_sec + elapsed_ms / 1000LL);
    tv->tv_usec = (suseconds_t)((elapsed_ms % 1000LL) * 1000LL);
    return 0;
}

// ── Heap growth ───────────────────────────────────────────────────────────────
extern char _end[];
static char *heap_brk;

void *_sbrk(ptrdiff_t incr) {
    if (!heap_brk)
        heap_brk = _end;
    char *prev = heap_brk;
    heap_brk += incr;
    return (void *)prev;
}

// ── POSIX console stubs (fallback if picolibc's POSIX layer is used) ─────────
ssize_t _write(int fd, const void *buf, size_t len) {
    if (fd > 2) { errno = EBADF; return -1; }
    const char *p = (const char *)buf;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '\n') uart_putc('\r');
        uart_putc(p[i]);
    }
    return (ssize_t)len;
}

ssize_t _read(int fd, void *buf, size_t len) {
    if (fd != 0) { errno = EBADF; return -1; }
    if (len == 0) return 0;
    ((char *)buf)[0] = (char)uart_getc_blocking();
    return 1;
}

// ── Exit + no-op stubs ────────────────────────────────────────────────────────
void _exit(int code) {
    (void)code;
    __builtin_trap();
}

int _close(int fd)                           { (void)fd;  errno = ENOSYS; return -1; }
off_t _lseek(int fd, off_t off, int whence) { (void)fd; (void)off; (void)whence; errno = ENOSYS; return -1; }
int _fstat(int fd, struct stat *st)         { (void)fd; (void)st;  errno = ENOSYS; return -1; }
int _stat(const char *p, struct stat *st)   { (void)p;  (void)st;  errno = ENOSYS; return -1; }
int _isatty(int fd)                         { return fd <= 2 ? 1 : 0; }
int _getpid(void)                           { return 1; }
int _kill(int pid, int sig)                 { (void)pid; (void)sig; errno = EINVAL; return -1; }
