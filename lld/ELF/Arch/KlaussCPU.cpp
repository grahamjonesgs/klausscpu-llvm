//===- KlaussCPU.cpp ------------------------------------------------------===//
//
// LLD ELF backend for KlaussCPU — a home-designed 64-bit CPU on FPGA.
//
// ELF: ELFCLASS32 / ELFDATA2LSB / EM_KLAUSSCPU (0x4B43)
//
// One relocation type:
//   R_KLAUSSCPU_ABS32 (1) — 32-bit absolute address, big-endian, written
//   into bytes [loc+0..loc+3].  Used for branch/call targets (word 1 of
//   an 8-byte Vcall/Vbr instruction).
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
    // 32-bit absolute address, big-endian (instruction word 1).
    checkUInt(ctx, loc, val, 32, rel);
    write32be(loc, static_cast<uint32_t>(val));
    break;
  default:
    llvm_unreachable("unrecognized KlaussCPU relocation");
  }
}

void elf::setKlaussCPUTargetInfo(Ctx &ctx) {
  ctx.target.reset(new KlaussCPU(ctx));
}
