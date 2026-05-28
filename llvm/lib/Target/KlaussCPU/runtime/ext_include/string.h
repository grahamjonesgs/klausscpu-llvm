/* ext_include/string.h — minimal string/memory decls for KlaussCPU LLEXT
 * extensions.  Resolved at load time against the kernel's exported libc. */
#ifndef _EXT_STRING_H
#define _EXT_STRING_H

#include <stddef.h>

void  *memcpy(void *dest, const void *src, size_t n);
void  *memmove(void *dest, const void *src, size_t n);
void  *memset(void *s, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dest, const char *src);
char  *strncpy(char *dest, const char *src, size_t n);
char  *strcat(char *dest, const char *src);
char  *strchr(const char *s, int c);

#endif /* _EXT_STRING_H */
