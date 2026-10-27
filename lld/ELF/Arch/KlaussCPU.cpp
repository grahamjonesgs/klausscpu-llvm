//===- KlaussCPU.cpp ------------------------------------------------------===//
//
// LLD ELF backend for KlaussCPU — a home-designed 64-bit CPU on FPGA.
//
// ELF: ELFCLASS32 / ELFDATA2LSB / EM_KLAUSSCPU (0x4B43)
//
// Relocation types:
//   R_KLAUSSCPU_ABS32  (1) — 32-bit absolute, LE — instruction word-1 slot
//                             (branch/call target) or 4-byte data reference.
//   R_KLAUSSCPU_ABS64  (2) — 64-bit absolute, LE — 8-byte pointer field in
//                             .rodata/.data (struct members, pointer arrays):
//                             S + A in 64 bits, the address S zero-extended.
//   R_KLAUSSCPU_PCREL32(3) — 32-bit signed PC-relative, LE — word-1 slot of
//                             JMPREL/JMPxxREL/CALLREL/LEAPC instructions.
//                             field_value = S + A − instr_addr.
//                             lld computes S + A − (instr_addr+4) (R_PC) and
//                             we add 4 in relocate() for the hardware convention.
//   R_KLAUSSCPU_PC32   (4) — 32-bit signed PC-relative *data* — a cross-section
//                             symbol difference (A − B) in .rodata/.data, e.g.
//                             the -fPIC relative lookup tables emitted by
//                             RelLookupTableConverter (`.long str - table`).
//                             field_value = S + A − P exactly (NO +4: unlike the
//                             instruction form, the fixup slot IS the PC origin).
//   R_KLAUSSCPU_PC64   (5) — 64-bit PC-relative data, as PC32 for an 8-byte
//                             field (`.quad A − B`, e.g. InstrProf's
//                             __profd_* CounterPtr).
//
// Only static links are supported: there are no dynamic relocation types.
// Code loaded at run time is relocated by its loader, from an ET_REL object
// (Zephyr llext) or from the RELA sections kept by --emit-relocs.
//
//===----------------------------------------------------------------------===//

#include "Symbols.h"
#include "Target.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Endian.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace llvm::ELF;
using namespace lld;
using namespace lld::elf;

namespace {
class KlaussCPU final : public TargetInfo {
public:
  KlaussCPU(Ctx &);
  RelExpr getRelExpr(RelType type, const Symbol &s,
                     const uint8_t *loc) const override;
  void relocate(uint8_t *loc, const Relocation &rel,
                uint64_t val) const override;
};
} // namespace

KlaussCPU::KlaussCPU(Ctx &ctx) : TargetInfo(ctx) {
  // Fill between input sections: the retired v1 HALT encoding (0x0000F011,
  // big-endian).  Its LEN field [31:30] of the LE word 0x11F00000 is 00, so
  // the v2 CPU raises ERR_INV_OPCODE if it is ever executed.
  trapInstr = {0x00, 0x00, 0xF0, 0x11};
}

RelExpr KlaussCPU::getRelExpr(RelType type, const Symbol &s,
                               const uint8_t *loc) const {
  switch (type) {
  case R_KLAUSSCPU_NONE:
    return R_NONE;
  case R_KLAUSSCPU_ABS32:
  case R_KLAUSSCPU_ABS64:
    return R_ABS;
  case R_KLAUSSCPU_PCREL32:
  case R_KLAUSSCPU_PC32:
  case R_KLAUSSCPU_PC64:
    return R_PC;
  default:
    Err(ctx) << getErrorLoc(ctx, loc) << "unrecognized relocation " << type;
    return R_NONE;
  }
}

void KlaussCPU::relocate(uint8_t *loc, const Relocation &rel,
                          uint64_t val) const {
  switch (rel.type) {
  case R_KLAUSSCPU_ABS32:
    // Unsigned address, but also accept -1 (the .debug_* tombstone).
    checkIntUInt(ctx, loc, val, 32, rel);
    write32le(loc, static_cast<uint32_t>(val));
    break;
  case R_KLAUSSCPU_ABS64:
    // 8-byte field in .rodata/.data: the exact 64-bit S + A, with the 32-bit
    // address S zero-extended as SETRU, LEAPC and the assembler's own
    // FK_Data_8 build it.  lld hands an ELFCLASS32 target `val` sign-extended
    // from 32 bits, which cannot tell an address >= 2 GiB (positive) from a
    // negative S + A (`(intptr_t)&x - 0x100000` with x at a low address), so
    // recompute it from the symbol: S + A in 64 bits, as the code computes
    // it (KlaussCPUISelLowering keeps a negative offset out of SETRU).
    if (rel.expr == R_ABS && rel.sym) {
      write64le(loc, rel.sym->getVA(ctx, rel.addend));
      break;
    }
    // A non-alloc section (relocateNonAlloc: expr R_NONE, no symbol), i.e.
    // .debug_*, where S + A is an address: zero-extend the low half, except
    // the all-ones tombstone of a reference to a discarded section, which
    // DWARF consumers expect.  (A live reference to exactly 0xFFFFFFFF also
    // reaches here as all-ones; relocateNoSym passes nothing to tell them
    // apart; nothing the compiler describes in DWARF lives there.)
    if (val == UINT64_MAX)
      write64le(loc, val);
    else
      write64le(loc, static_cast<uint32_t>(val));
    break;
  case R_KLAUSSCPU_PCREL32:
    // val = S + A − (instr_addr+4) from lld's R_PC computation.
    // Hardware uses instr_addr as PC origin, so field = val + 4.
    checkInt(ctx, loc, static_cast<int64_t>(val) + 4, 32, rel);
    write32le(loc, static_cast<uint32_t>(val + 4));
    break;
  case R_KLAUSSCPU_PC32:
    // Data symbol difference (A − B): the fixup slot itself is the PC origin,
    // so the field is exactly S + A − P — no instruction "+4" correction.
    checkInt(ctx, loc, static_cast<int64_t>(val), 32, rel);
    write32le(loc, static_cast<uint32_t>(val));
    break;
  case R_KLAUSSCPU_PC64:
    // val is already sign-extended from 32 bits (ELFCLASS32), which is
    // exactly the 64-bit difference.
    write64le(loc, val);
    break;
  default:
    llvm_unreachable("unrecognized KlaussCPU relocation");
  }
}

void elf::setKlaussCPUTargetInfo(Ctx &ctx) {
  ctx.target.reset(new KlaussCPU(ctx));
}
