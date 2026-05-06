// stdio_handles.c — writable FILE* globals and stdio initialization.
//
// Intentionally does NOT include <stdio.h>.  picolibc declares stdin/stdout/stderr
// as 'extern FILE * const', which prevents assignment in any TU that includes it.
// We define the ELF symbols here as plain 'FILE *' (writable) — the const qualifier
// is purely a C-level hint and has no effect on ELF symbol resolution.
//
// __stdio_init() is called from crt0 before main().  All stores go through .text
// SETR+STIDX64 instructions (R_KLAUSSCPU_ABS32 text-section relocation), avoiding
// the data-section 64-bit pointer fixups that our ELF backend does not yet emit.
//
// Struct layout verified against the installed picolibc (symbol size = 32 bytes):
//   offset  0: unget  (uint16_t, __ungetc_t)
//   offset  2: flags  (uint8_t)
//   offset  3: padding [5 bytes for 8-byte pointer alignment]
//   offset  8: put    (int (*)(char, struct __file *))
//   offset 16: get    (int (*)(struct __file *))
//   offset 24: flush  (int (*)(struct __file *))
//   total:     32 bytes

struct __file {
    unsigned short unget;
    unsigned char  flags;
    unsigned char  _pad[5];
    int (*put)(char, struct __file *);
    int (*get)(struct __file *);
    int (*flush)(struct __file *);
};
typedef struct __file FILE;

#define _FDEV_SETUP_WRITE 0x02
#define _FDEV_SETUP_READ  0x01

// UART callbacks defined in syscalls.c
extern int _uart_put(char c, FILE *f);
extern int _uart_get(FILE *f);

// FILE objects in .bss (zeroed by crt0 before __stdio_init runs)
static FILE _stdout_file;
static FILE _stdin_file;
static FILE _stderr_file;

// The FILE* globals picolibc resolves.  Non-const here; picolibc sees them as
// 'FILE * const' from its own header, but never writes through them.
FILE *stdout;
FILE *stdin;
FILE *stderr;

// Called from crt0.c immediately before main().
void __stdio_init(void) {
    _stdout_file.put   = _uart_put;
    _stdout_file.flags = _FDEV_SETUP_WRITE;

    _stdin_file.get    = _uart_get;
    _stdin_file.flags  = _FDEV_SETUP_READ;

    _stderr_file.put   = _uart_put;
    _stderr_file.flags = _FDEV_SETUP_WRITE;

    stdout = &_stdout_file;
    stdin  = &_stdin_file;
    stderr = &_stderr_file;
}
