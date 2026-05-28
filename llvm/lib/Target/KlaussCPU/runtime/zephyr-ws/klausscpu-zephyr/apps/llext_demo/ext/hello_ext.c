/*
 * hello_ext.c — minimal KlaussCPU LLEXT extension.
 *
 * Built standalone with the KlaussCPU clang into an ELFCLASS32 ET_REL object,
 * then embedded as a byte array (hello_ext_bin.inc) into the demo app.  See
 * apps/llext_demo/README or the Makefile-style command in CMakeLists for the
 * exact compile invocation.
 *
 * `hello_world` is an ordinary GLOBAL function, so the loader records it in the
 * extension's symbol table (llext_copy_symbols) and the app resolves it with
 * llext_find_sym(&ext->sym_tab, "hello_world") — no LL_EXTENSION_SYMBOL needed.
 *
 * `printk` is resolved against the kernel's EXPORT_SYMBOL table at load time.
 */
extern int printk(const char *fmt, ...);

int hello_count;   /* .bss global — exercises an ABS32 reloc against .bss */

void hello_world(void)
{
	printk("Hello from llext! (call #%d)\n", ++hello_count);
}
