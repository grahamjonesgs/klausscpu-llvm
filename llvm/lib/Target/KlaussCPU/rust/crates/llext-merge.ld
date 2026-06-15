/* Relocatable linker script for `ld.lld -r`: merge Rust's per-function
 * sections into single .text/.rodata/.data/.bss outputs.
 *
 * Rust defaults to function-sections (one .text.<mangled> per function), so a
 * Rust .llext otherwise has 100+ loaded sections — each function at offset 0
 * of its own section, every internal call a relocation against that section
 * symbol. The KlaussCPU Zephyr LLEXT loader was only ever exercised on C
 * extensions, which have a single .text; with many sections an internal call
 * resolves to (section_base=0)+(addend=0)=0 → PC=0 crash.
 *
 * Merging makes a Rust .llext structurally identical to a C one (one .text,
 * one .rodata), so internal calls become intra-section against the single
 * .text symbol the loader handles correctly. Used after member selection
 * by the per-example build-llext.sh scripts.
 */
SECTIONS {
  .text   : { *(.text .text.*) }
  .rodata : { *(.rodata .rodata.* .data.rel.ro .data.rel.ro.*) }
  .data   : { *(.data .data.*) }
  .bss    : { *(.bss .bss.* COMMON) }
}
