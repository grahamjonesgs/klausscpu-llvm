//===- KlaussCPU.cpp ------------------------------------------------------===//
//
// LLD ELF backend for KlaussCPU — a home-designed 64-bit CPU on FPGA.
//
// ELF: ELFCLASS32 / ELFDATA2LSB / EM_KLAUSSCPU (0x4B43)
//
// Relocation types:
//   R_KLAUSSCPU_ABS32 (1) — 32-bit absolute, LE — instruction word-1 slot
//                            (branch/call target) or 4-byte data reference.
//   R_KLAUSSCPU_ABS64 (2) — 64-bit absolute, LE — 8-byte pointer field in
//                            .rodata/.data (struct members, pointer arrays).
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
  // HALT instruction: 0x0000F011 big-endian
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
  default:
    Err(ctx) << getErrorLoc(ctx, loc) << "unrecognized relocation " << type;
    return R_NONE;
  }
}

void KlaussCPU::relocate(uint8_t *loc, const Relocation &rel,
                          uint64_t val) const {
  switch (rel.type) {
  case R_KLAUSSCPU_ABS32:
    checkUInt(ctx, loc, val, 32, rel);
    write32le(loc, static_cast<uint32_t>(val));
    break;
  case R_KLAUSSCPU_ABS64:
    // 8-byte pointer field in .rodata/.data — all addresses fit in 32 bits,
    // upper 32 bits are always zero.
    write64le(loc, val);
    break;
  default:
    llvm_unreachable("unrecognized KlaussCPU relocation");
  }
}

void elf::setKlaussCPUTargetInfo(Ctx &ctx) {
  ctx.target.reset(new KlaussCPU(ctx));
}
