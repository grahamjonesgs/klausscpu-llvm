/*
 * elf.c — KlaussCPU architecture-specific LLEXT relocations.
 *
 * PROPOSAL / NOT YET WIRED INTO THE BUILD.
 * This implements the arch hook that Zephyr's LLEXT subsystem
 * (subsys/llext/) calls to relocate a partially-linked (ET_REL) extension
 * after it has been copied into RAM.  It is the runtime-loader counterpart
 * of lld/ELF/Arch/KlaussCPU.cpp::relocate() — the formulas here MUST match
 * lld so a loaded extension behaves identically to a statically linked one.
 *
 * Targets Zephyr 3.7 LTS, whose hook signature is:
 *     int arch_elf_relocate(elf_rela_t *rel, uintptr_t loc,
 *                           uintptr_t sym_base_addr, const char *sym_name,
 *                           uintptr_t load_bias);
 * (Newer Zephyr changed this to (ldr, ext, rel, shdr) and does symbol
 *  lookup inside the arch hook — see arch/riscv/core/elf.c on main.)
 *
 * KlaussCPU relocations are whole-field (no HI20/LO12 splitting, no
 * instruction-immediate bit-packing), so unlike RISC-V/ARM this handler is
 * tiny: three real cases.
 *
 * Endianness: KlaussCPU data memory is little-endian, and lld writes every
 * relocated field (including the instruction word-1 immediate slot) with
 * write32le/write64le.  We therefore use sys_put_le32/64 so the bytes match
 * lld exactly and unaligned targets are handled safely.
 */

#include <errno.h>

#include <zephyr/llext/elf.h>
#include <zephyr/llext/llext.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(elf, CONFIG_LLEXT_LOG_LEVEL);

/*
 * KlaussCPU relocation types — mirror of the values in
 * llvm/include/llvm/BinaryFormat/ELFRelocs/KlaussCPU.def and the loader in
 * runtime/programs/loader.c.  (No <zephyr/arch/klausscpu/elf.h> exists yet;
 * if LLEXT support is adopted, move these there alongside an EM_KLAUSSCPU
 * definition.)
 */
#define R_KLAUSSCPU_NONE    0
#define R_KLAUSSCPU_ABS32   1
#define R_KLAUSSCPU_ABS64   2
#define R_KLAUSSCPU_PCREL32 3

/*
 * PC-relative fields are stored relative to the START of the instruction,
 * while the relocation r_offset (and therefore `loc`) points at the 4-byte
 * immediate slot, which lives at offset 4 within the 8-byte instruction.
 * lld encodes this as: stored = S + A - P + 4  (see KlaussCPU.cpp relocate()).
 */
#define KLAUSSCPU_PCREL_INSN_BIAS 4

/**
 * @brief Relocate one entry of a partially-linked KlaussCPU extension.
 *
 * Symbolic names follow the usual ELF convention:
 *   S = sym_base_addr   (resolved address of the referenced symbol)
 *   A = rel->r_addend   (RELA addend; KlaussCPU emits .rela sections)
 *   P = loc             (address of the field being patched)
 */
int arch_elf_relocate(elf_rela_t *rel, uintptr_t loc, uintptr_t sym_base_addr,
		      const char *sym_name, uintptr_t load_bias)
{
	uint32_t reloc_type = ELF32_R_TYPE(rel->r_info);

	ARG_UNUSED(load_bias); /* RELA gives absolute S+A / self-relative P; no bias needed */

	LOG_DBG("reloc type %u  loc %#lx  S %#lx  A %ld  sym '%s'", reloc_type,
		(unsigned long)loc, (unsigned long)sym_base_addr,
		(long)rel->r_addend, sym_name ? sym_name : "?");

	switch (reloc_type) {
	case R_KLAUSSCPU_NONE:
		break;

	case R_KLAUSSCPU_ABS32:
		/* 32-bit absolute (e.g. SETR sym immediate).  S + A. */
		sys_put_le32((uint32_t)(sym_base_addr + rel->r_addend),
			     (uint8_t *)loc);
		break;

	case R_KLAUSSCPU_ABS64:
		/* 64-bit absolute (8-byte data pointer field).  S + A. */
		sys_put_le64((uint64_t)(sym_base_addr + rel->r_addend),
			     (uint8_t *)loc);
		break;

	case R_KLAUSSCPU_PCREL32:
		/* PC-relative call/branch target.  S + A - P, biased +4 so the
		 * field is relative to the instruction start (hardware
		 * convention; identical to lld's relocate()). */
		sys_put_le32((uint32_t)(sym_base_addr + rel->r_addend - loc +
					KLAUSSCPU_PCREL_INSN_BIAS),
			     (uint8_t *)loc);
		break;

	default:
		LOG_ERR("unsupported relocation type %u for symbol '%s'",
			reloc_type, sym_name ? sym_name : "?");
		return -ENOTSUP;
	}

	return 0;
}
