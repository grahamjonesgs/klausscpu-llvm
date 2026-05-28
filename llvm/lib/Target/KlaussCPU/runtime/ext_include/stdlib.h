/* ext_include/stdlib.h — minimal stdlib for KlaussCPU LLEXT extensions.
 * malloc family resolves at load time against the kernel's common-libc heap
 * (CONFIG_COMMON_LIBC_MALLOC). */
#ifndef _EXT_STDLIB_H
#define _EXT_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

#endif /* _EXT_STDLIB_H */
