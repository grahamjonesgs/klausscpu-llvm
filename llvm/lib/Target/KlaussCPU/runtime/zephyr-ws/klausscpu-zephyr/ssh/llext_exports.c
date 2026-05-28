/*
 * llext_exports.c — kernel symbols exported to loadable extensions.
 *
 * Extensions (former PIC demo programs, now built as ELFCLASS32 ET_REL
 * objects) call the standard C library directly; the llext loader resolves
 * those UND references against this table.  The functions themselves are the
 * kernel's own minimal/common libc, so a single libc is shared between the
 * kernel and every extension.
 *
 * Console routing: printf/puts/putchar funnel through arch_printk_char_out(),
 * which the run loader redirects to the SSH session.  getchar() has no
 * minimal-libc implementation, so it is provided here and reads through the
 * input-redirect hook the loader installs.
 */

#include <zephyr/llext/symbol.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Minimal libc's <stdio.h> macro-defines putchar(c) as putc(c, stdout); undo
 * that so EXPORT_SYMBOL(putchar) refers to the real function defined in
 * lib/libc/minimal/source/stdout/stdout_console.c.
 */
#undef putchar
extern int putchar(int c);

/*
 * Console input redirect, mirror of klausscpu_console_out_hook (in
 * arch/klausscpu/core/irq.c).  NULL when no extension is running; otherwise
 * set by the run loader to pull bytes from the active SSH session.
 */
int (*klausscpu_console_in_hook)(void);

int getchar(void)
{
	int (*hook)(void) = klausscpu_console_in_hook;

	if (hook != NULL) {
		return hook();
	}

	/* No bare-console input path is wired in this build. */
	return EOF;
}

/* stdio */
EXPORT_SYMBOL(printf);
EXPORT_SYMBOL(snprintf);
EXPORT_SYMBOL(puts);
EXPORT_SYMBOL(putchar);
EXPORT_SYMBOL(getchar);

/* stdlib — malloc family (CONFIG_COMMON_LIBC_MALLOC) */
EXPORT_SYMBOL(malloc);
EXPORT_SYMBOL(calloc);
EXPORT_SYMBOL(realloc);
EXPORT_SYMBOL(free);

/* string / memory */
EXPORT_SYMBOL(memcpy);
EXPORT_SYMBOL(memmove);
EXPORT_SYMBOL(memset);
EXPORT_SYMBOL(memcmp);
EXPORT_SYMBOL(strlen);
EXPORT_SYMBOL(strcmp);
EXPORT_SYMBOL(strncmp);
EXPORT_SYMBOL(strcpy);
EXPORT_SYMBOL(strncpy);
EXPORT_SYMBOL(strcat);
EXPORT_SYMBOL(strchr);
