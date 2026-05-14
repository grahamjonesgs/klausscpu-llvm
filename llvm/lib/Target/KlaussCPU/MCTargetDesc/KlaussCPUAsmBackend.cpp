//===-- KlaussCPUAsmBackend.cpp - KlaussCPU assembler backend --------------===//
//
// Implements MCAsmBackend for KlaussCPU:
//
//   FK_KlaussCPU_ABS32   — 32-bit absolute, LE, placed at byte offset 4–7 of
//     an 8-byte branch/call/SETR instruction (word 1).
//
//   FK_KlaussCPU_PCREL32 — 32-bit signed PC-relative offset, LE, same word-1
//     slot.  field_value = target − PC_of_instruction (option B convention).
//     LLVM's FKF_IsPCRel causes it to subtract the fixup address (instr+4);
//     we add 4 back in applyFixup / lld relocate to correct for the hardware
//     convention where PC origin is instr_addr (not instr_addr+4).
//
//   ELF output: ELFCLASS32, ELFDATA2LSB, EM_KLAUSSCPU.
//   No instruction relaxation.
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
                                 /*EMachine=*/ELF::EM_KLAUSSCPU,
                                 /*HasRelocationAddend=*/true) {}

  unsigned getRelocType(const MCFixup &Fixup, const MCValue &Target,
                        bool IsPCRel) const override {
    if (Fixup.getKind() == FK_Data_8)
      return ELF::R_KLAUSSCPU_ABS64;
    if (Fixup.getKind() == MCFixupKind(KlaussCPU::FK_KlaussCPU_PCREL32))
      return ELF::R_KLAUSSCPU_PCREL32;
    return ELF::R_KLAUSSCPU_ABS32;
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
        // Name                    BitOffset  BitSize  Flags
        {"FK_KlaussCPU_ABS32",     0,         32,      0},
        {"FK_KlaussCPU_PCREL32",   0,         32,      0},
    };
    if (Kind >= FirstTargetFixupKind &&
        Kind < FirstTargetFixupKind + KlaussCPU::NumTargetFixupKinds)
      return Infos[Kind - FirstTargetFixupKind];
    return MCAsmBackend::getFixupKindInfo(Kind);
  }

  // ── Fixup application ─────────────────────────────────────────────────────

  // Data points to the first byte of the fixup location (byte 4 of the 8-byte
  // branch/call instruction, as specified by MCFixup offset=4 in the emitter).
  void applyFixup(const MCFragment &F, const MCFixup &Fixup,
                  const MCValue &Target, uint8_t *Data,
                  uint64_t Value, bool IsResolved) override {
    MCFixupKind Kind = Fixup.getKind();
    // FK_Data_8: 8-byte pointer field in .rodata/.data (memp_pools[], struct
    // initializers with pointer members, etc.).  Stored little-endian — the
    // data bus is LE.  All KlaussCPU addresses fit in 32 bits; upper 32 = 0.
    if (Kind == FK_Data_8) {
      for (int i = 0; i < 8; ++i)
        Data[i] = (Value >> (8 * i)) & 0xFF;
      maybeAddReloc(F, Fixup, Target, Value, IsResolved);
      return;
    }
    // FK_KlaussCPU_PCREL32: signed PC-relative offset in word-1 slot.
    // LLVM 23 has no FKF_IsPCRel, so the assembler passes raw target_addr as
    // Value without subtracting the PC.  We always defer to the linker which
    // computes the correct field_value = S + A − instr_addr (with +4 adjust
    // in KlaussCPU.cpp relocate() for the hardware PC convention).
    // Write 0 as placeholder; R_KLAUSSCPU_PCREL32 reloc carries the symbol.
    if (Kind == MCFixupKind(KlaussCPU::FK_KlaussCPU_PCREL32)) {
      Data[0] = Data[1] = Data[2] = Data[3] = 0;
      maybeAddReloc(F, Fixup, Target, Value, IsResolved);
      return;
    }
    // FK_KlaussCPU_ABS32 / FK_Data_4: 32-bit LE address — instruction word-1
    // slot (CALL/SETR) or 4-byte data reference (EK_Custom32 jump tables).
    if (Kind != KlaussCPU::FK_KlaussCPU_ABS32 && Kind != FK_Data_4)
      return;
    uint32_t Addr = static_cast<uint32_t>(Value);
    Data[0] = (Addr >>  0) & 0xFF;
    Data[1] = (Addr >>  8) & 0xFF;
    Data[2] = (Addr >> 16) & 0xFF;
    Data[3] = (Addr >> 24) & 0xFF;
    maybeAddReloc(F, Fixup, Target, Value, IsResolved);
  }

  // ── NOP padding ──────────────────────────────────────────────────────────

  // NOP_I = 0x0000F010 (4-byte, little-endian: 10 F0 00 00).
  // KlaussCPU instructions are 4-byte aligned; reject sub-word padding.
  bool writeNopData(raw_ostream &OS, uint64_t Count,
                    const MCSubtargetInfo *STI) const override {
    if (Count % 4 != 0)
      return false;
    for (uint64_t i = 0, n = Count / 4; i < n; ++i)
      OS.write("\x10\xF0\x00\x00", 4);
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
