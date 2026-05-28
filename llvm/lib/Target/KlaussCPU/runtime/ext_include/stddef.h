/* ext_include/stddef.h — minimal stddef for KlaussCPU LLEXT extensions.
 * Compiled with -nostdinc; types come from the compiler's own builtins so
 * they always match the target ABI (size_t = unsigned long on KlaussCPU). */
#ifndef _EXT_STDDEF_H
#define _EXT_STDDEF_H

typedef __SIZE_TYPE__    size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)

#endif /* _EXT_STDDEF_H */
