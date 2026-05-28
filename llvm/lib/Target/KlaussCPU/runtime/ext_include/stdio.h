/* ext_include/stdio.h — minimal stdio for KlaussCPU LLEXT extensions.
 *
 * Plain extern declarations only — NO FILE machinery and NO putchar/getchar
 * macros (unlike picolibc, which expands putchar(c) to fputc(c, stdout)).
 * Each name resolves at load time to a kernel symbol exported in
 * ssh/llext_exports.c, so extensions share the kernel's single libc. */
#ifndef _EXT_STDIO_H
#define _EXT_STDIO_H

#include <stddef.h>

#define EOF (-1)

int printf(const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...);
int puts(const char *s);
int putchar(int c);
int getchar(void);

#endif /* _EXT_STDIO_H */
