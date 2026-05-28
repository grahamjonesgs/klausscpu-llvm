/*
 * KlaussCPU LLEXT demo — loads an embedded ELFCLASS32 extension from RAM,
 * resolves and calls hello_world(), then unloads it.
 *
 * This exercises the full llext path on KlaussCPU:
 *   - ELF32 parsing under CONFIG_64BIT=y (CONFIG_LLEXT_ELF_CLASS32)
 *   - SHT_RELA relocation handling (llext_link.c generic path)
 *   - arch_elf_relocate() in arch/klausscpu/core/elf.c (ABS32/ABS64/PCREL32)
 *   - kernel symbol export resolution (EXPORT_SYMBOL(printk))
 */
#include <zephyr/kernel.h>
#include <zephyr/llext/llext.h>
#include <zephyr/llext/buf_loader.h>
#include <zephyr/llext/symbol.h>

#include "hello_ext_bin.inc"

/* The extension references printk; expose it to the loader's symbol table. */
EXPORT_SYMBOL(printk);

int main(void)
{
	printk("=== KlaussCPU LLEXT demo ===\n");
	printk("extension image: %u bytes\n", (unsigned int)sizeof(hello_ext_bin));

	struct llext_buf_loader buf_loader =
		LLEXT_BUF_LOADER(hello_ext_bin, sizeof(hello_ext_bin));
	struct llext_loader *ldr = &buf_loader.loader;
	struct llext_load_param ldr_parm = LLEXT_LOAD_PARAM_DEFAULT;
	struct llext *ext = NULL;

	/* Keep ext->sym_tab after load so llext_find_sym() can resolve by name. */
	ldr_parm.keep_symtab = true;

	int ret = llext_load(ldr, "hello", &ext, &ldr_parm);

	if (ret != 0) {
		printk("llext_load FAILED: %d\n", ret);
		return 0;
	}
	printk("llext_load OK (ext=%p)\n", (void *)ext);

	void (*fn)(void) =
		(void (*)(void))llext_find_sym(&ext->sym_tab, "hello_world");

	if (fn == NULL) {
		printk("symbol 'hello_world' not found in extension\n");
	} else {
		printk("calling hello_world @ %p\n", (void *)fn);
		fn();
		fn();
	}

	ret = llext_unload(&ext);
	printk("llext_unload: %d\n", ret);
	printk("=== LLEXT demo done ===\n");
	return 0;
}
