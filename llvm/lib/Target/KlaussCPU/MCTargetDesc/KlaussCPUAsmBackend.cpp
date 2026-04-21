//===-- KlaussCPUAsmBackend.cpp - KlaussCPU assembler backend --------------===//
//
// Implements MCAsmBackend for KlaussCPU:
//
//   Fixup kind
//     FK_KlaussCPU_ABS32 — 32-bit absolute address, big-endian, placed at
//     byte offset 4–7 of an 8-byte branch/call instruction (word 1).
//
//   ELF output
//     ELFCLASS32, ELFDATA2LSB (data bus is little-endian), EM_NONE.
//     Instruction bytes in .text are already big-endian from the emitter.
//
//   No instruction relaxation (all branch targets are fixed 32-bit absolutes).
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUFixupKinds.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCValue.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

// ── ELF object target writer ──────────────────────────────────────────────

namespace {

class KlaussCPUELFObjectWriter : public MCELFObjectTargetWriter {
public:
  KlaussCPUELFObjectWriter()
      : MCELFObjectTargetWriter(/*Is64Bit=*/false,
                                 /*OSABI=*/0,
                                 /*EMachine=*/ELF::EM_NONE,
                                 /*HasRelocationAddend=*/true) {}

  // R_KCPU_ABS32 = 1: custom 32-bit absolute-address relocation.
  unsigned getRelocType(const MCFixup &Fixup, const MCValue &Target,
                        bool IsPCRel) const override {
    return 1;
  }
};

// ── Assembler backend ─────────────────────────────────────────────────────

class KlaussCPUAsmBackend : public MCAsmBackend {
public:
  KlaussCPUAsmBackend()
      : MCAsmBackend(llvm::endianness::little) {}

  // ── Fixup kind info ───────────────────────────────────────────────────────

  MCFixupKindInfo getFixupKindInfo(MCFixupKind Kind) const override {
    static const MCFixupKindInfo Infos[] = {
        // Name                  BitOffset  BitSize  Flags
        {"FK_KlaussCPU_ABS32",   0,         32,      0},
    };
    if (Kind >= FirstTargetFixupKind &&
        Kind < FirstTargetFixupKind + KlaussCPU::NumTargetFixupKinds)
      return Infos[Kind - FirstTargetFixupKind];
    return MCAsmBackend::getFixupKindInfo(Kind);
  }

  // ── Fixup application ─────────────────────────────────────────────────────

  // Data points to the first byte of the fixup location (byte 4 of the 8-byte
  // branch/call instruction, as specified by MCFixup offset=4 in the emitter).
  // Write the resolved 32-bit address big-endian into bytes [0..3].
  void applyFixup(const MCFragment &, const MCFixup &Fixup,
                  const MCValue &Target, uint8_t *Data,
                  uint64_t Value, bool IsResolved) override {
    if (Fixup.getKind() != KlaussCPU::FK_KlaussCPU_ABS32)
      return;
    uint32_t Addr = static_cast<uint32_t>(Value);
    Data[0] = (Addr >> 24) & 0xFF;
    Data[1] = (Addr >> 16) & 0xFF;
    Data[2] = (Addr >>  8) & 0xFF;
    Data[3] = (Addr >>  0) & 0xFF;
  }

  // ── NOP padding ──────────────────────────────────────────────────────────

  // NOP_I = 0x0000F010 (4-byte, big-endian: 00 00 F0 10).
  // KlaussCPU instructions are 4-byte aligned; reject sub-word padding.
  bool writeNopData(raw_ostream &OS, uint64_t Count,
                    const MCSubtargetInfo *STI) const override {
    if (Count % 4 != 0)
      return false;
    for (uint64_t i = 0, n = Count / 4; i < n; ++i)
      OS.write("\x00\x00\xF0\x10", 4);
    return true;
  }

  // ── ELF object target writer ──────────────────────────────────────────────

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override {
    return std::make_unique<KlaussCPUELFObjectWriter>();
  }
};

} // anonymous namespace

// ── Factory ───────────────────────────────────────────────────────────────

namespace llvm {
MCAsmBackend *createKlaussCPUAsmBackend(const Target &T,
                                         const MCSubtargetInfo &STI,
                                         const MCRegisterInfo &MRI,
                                         const MCTargetOptions &Options) {
  return new KlaussCPUAsmBackend();
}
} // namespace llvm
