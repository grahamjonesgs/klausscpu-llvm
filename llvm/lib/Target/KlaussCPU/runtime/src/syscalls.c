// syscalls.c — KlaussCPU libgloss: connects picolibc to the hardware.
//
// Provides:
//  - stdin/stdout/stderr FILE objects backed by the UART (FDEV_SETUP_STREAM)
//  - _sbrk for heap growth (malloc)
//  - POSIX stubs (_write/_read/_exit/etc.) as fallback
//
// Build: compiled as part of every program, same flags as the program itself.

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <stddef.h>

// ── UART primitives from uart_stubs.c ────────────────────────────────────────
extern void               uart_putc(char c);
extern unsigned long long uart_getc_blocking(void);

// ── FILE-backed stdin / stdout / stderr ──────────────────────────────────────
// picolibc declares stdin/stdout/stderr as `extern FILE * const` and expects
// the application to define the FILE objects and the pointers.

static int _uart_put(char c, FILE *f) {
    (void)f;
    if (c == '\n') uart_putc('\r');  // CR+LF for serial terminals
    uart_putc(c);
    return 0;
}

static int _uart_get(FILE *f) {
    (void)f;
    return (int)(unsigned char)uart_getc_blocking();
}

static FILE _stdout_file = FDEV_SETUP_STREAM(_uart_put, NULL, NULL, _FDEV_SETUP_WRITE);
static FILE _stdin_file  = FDEV_SETUP_STREAM(NULL, _uart_get, NULL, _FDEV_SETUP_READ);
static FILE _stderr_file = FDEV_SETUP_STREAM(_uart_put, NULL, NULL, _FDEV_SETUP_WRITE);

FILE * const stdout = &_stdout_file;
FILE * const stdin  = &_stdin_file;
FILE * const stderr = &_stderr_file;

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
