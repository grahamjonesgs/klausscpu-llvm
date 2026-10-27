//===-- KlaussCPUAsmBackend.cpp - KlaussCPU assembler backend --------------===//
//
// Implements MCAsmBackend for KlaussCPU:
//
//   FK_KlaussCPU_ABS32   — 32-bit absolute, LE, placed at byte offset 4–7 of
//     an 8-byte branch/call/SETR(U) instruction (word 1).
//
//   FK_KlaussCPU_PCREL32 — 32-bit signed PC-relative offset, LE, same word-1
//     slot.  field_value = target − PC_of_instruction: the hardware adds the
//     field to the address of the instruction itself (pipeline_core.sv U_LEA
//     and brel: ex_pc + ex_var1).  The fixup is created PC-relative at offset
//     4, so MC hands applyFixup target − (instr+4); we add 4 back.  lld does
//     the same for R_KLAUSSCPU_PCREL32 (S + A − P + 4, P = instr+4).
//
//   FK_KlaussCPU_PCREL18 — ISA v3 A1 short branch: simm18 word displacement
//     in word0[17:0], target = PC + 4*disp (fixup at offset 0, IsPCRel, so
//     Value = target - PC exactly). Resolved at assembly time only: a short
//     branch that is out of range or not resolvable in-section is RELAXED to
//     the 2-word PC-relative form, so it never reaches the object writer.
//
//   FK_Data_{1,2,4,8} — data directives.  4- and 8-byte data have absolute
//     (ABS32/ABS64) and PC-relative (PC32/PC64, no instruction +4) relocations;
//     1- and 2-byte data must resolve at assembly time.
//
//   ELF output: ELFCLASS32, ELFDATA2LSB, EM_KLAUSSCPU.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUFixupKinds.h"
#include "KlaussCPUMCTargetDesc.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
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
    switch (Fixup.getKind()) {
    case FK_Data_4:
      // A cross-section symbol difference (A - B) reaches us as a PC-relative
      // data fixup.  The prime source is the relative lookup tables the
      // RelLookupTableConverter emits under -fPIC (`.long <str> - <table>`).
      // These need a *data* PC-relative relocation — value = S + A - P
      // exactly, WITHOUT the instruction word-1 "+4" of R_KLAUSSCPU_PCREL32.
      return IsPCRel ? ELF::R_KLAUSSCPU_PC32 : ELF::R_KLAUSSCPU_ABS32;
    case FK_Data_8:
      // `.quad A - B` across sections, e.g. InstrProf's __profd_* CounterPtr.
      return IsPCRel ? ELF::R_KLAUSSCPU_PC64 : ELF::R_KLAUSSCPU_ABS64;
    case FK_Data_1:
    case FK_Data_2:
      reportError(Fixup.getLoc(), Twine(Fixup.getKind() == FK_Data_1 ? 1 : 2) +
                                      "-byte data relocations not supported");
      return ELF::R_KLAUSSCPU_NONE;
    default:
      break;
    }
    switch (unsigned(Fixup.getKind())) {
    case KlaussCPU::FK_KlaussCPU_ABS32:
      if (IsPCRel) {
        // `setr r1, sym - .` etc.: an instruction's absolute word-1 slot has
        // no PC-relative relocation (use leapc).
        reportError(Fixup.getLoc(),
                    "PC-relative expression in an absolute operand");
        return ELF::R_KLAUSSCPU_NONE;
      }
      return ELF::R_KLAUSSCPU_ABS32;
    case KlaussCPU::FK_KlaussCPU_PCREL32:
      return ELF::R_KLAUSSCPU_PCREL32;
    default:
      reportError(Fixup.getLoc(), "unsupported relocation type");
      return ELF::R_KLAUSSCPU_NONE;
    }
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
        {"FK_KlaussCPU_PCREL18",   0,         18,      0},
        {"FK_KlaussCPU_PCREL13",   8,         13,      0},
    };
    if (Kind >= FirstTargetFixupKind &&
        Kind < FirstTargetFixupKind + KlaussCPU::NumTargetFixupKinds)
      return Infos[Kind - FirstTargetFixupKind];
    return MCAsmBackend::getFixupKindInfo(Kind);
  }

  // ── Fixup application ─────────────────────────────────────────────────────

  // Data points to the first byte of the fixup location (byte 4 of the 8-byte
  // instruction for word-1 fixups, as specified by MCFixup offset=4 in the
  // emitter).
  void applyFixup(const MCFragment &F, const MCFixup &Fixup,
                  const MCValue &Target, uint8_t *Data,
                  uint64_t Value, bool IsResolved) override {
    MCFixupKind Kind = Fixup.getKind();
    // FK_KlaussCPU_PCREL18: short branch, word displacement into word0[17:0].
    // Only ever applied resolved and in range (see fixupNeedsRelaxationAdvanced).
    if (Kind == MCFixupKind(KlaussCPU::FK_KlaussCPU_PCREL18)) {
      if (!IsResolved) {
        getContext().reportError(Fixup.getLoc(),
                                 "short branch target not resolvable");
        return;
      }
      uint32_t Disp = static_cast<uint32_t>(static_cast<int64_t>(Value) >> 2) &
                      0x3FFFFu;
      Data[0] |= Disp & 0xFF;
      Data[1] |= (Disp >> 8) & 0xFF;
      Data[2] |= (Disp >> 16) & 0x03;
      return;
    }
    // FK_KlaussCPU_PCREL13: fused branch, word displacement into word0[20:8].
    if (Kind == MCFixupKind(KlaussCPU::FK_KlaussCPU_PCREL13)) {
      int64_t V = static_cast<int64_t>(Value);
      if (!IsResolved || (V & 3) || !isInt<13>(V >> 2)) {
        getContext().reportError(Fixup.getLoc(),
                                 "fused branch target out of range");
        return;
      }
      uint32_t D = static_cast<uint32_t>(V >> 2) & 0x1FFFu;
      Data[1] |= D & 0xFF;          // word0[15:8]
      Data[2] |= (D >> 8) & 0x1F;   // word0[20:16]
      return;
    }

    // A PC-relative relocation already subtracts P; it cannot subtract a
    // second symbol (`leapc r1, ext - label`).
    if (!IsResolved && Fixup.isPCRel() && Target.getSubSym()) {
      getContext().reportError(Fixup.getLoc(),
                               "symbol difference in a PC-relative operand");
      return;
    }

    // ELF32 r_addend is a signed 32-bit field.  A 32-bit field only needs the
    // addend modulo 2^32; an 8-byte one sign-extends it.
    if (!IsResolved) {
      int64_t Addend = Target.getConstant();
      if (!isInt<32>(Addend) && (Kind == FK_Data_8 || !isUInt<32>(Addend))) {
        getContext().reportError(Fixup.getLoc(),
                                 "relocation addend out of range");
        return;
      }
    }

    // Record the relocation first: for RELA output it zeroes Value, so the
    // addend lives only in r_addend and not also in the section contents.
    maybeAddReloc(F, Fixup, Target, Value, IsResolved);

    unsigned NumBytes;
    switch (unsigned(Kind)) {
    case FK_Data_1:
      NumBytes = 1;
      break;
    case FK_Data_2:
      NumBytes = 2;
      break;
    // FK_KlaussCPU_ABS32 / FK_Data_4: 32-bit LE address — instruction word-1
    // slot (CALL/SETR(U)) or 4-byte data reference (EK_Custom32 jump tables).
    case FK_Data_4:
    case KlaussCPU::FK_KlaussCPU_ABS32:
      NumBytes = 4;
      break;
    // FK_Data_8: 8-byte pointer field in .rodata/.data (memp_pools[], struct
    // initializers with pointer members, etc.).
    case FK_Data_8:
      NumBytes = 8;
      break;
    // FK_KlaussCPU_PCREL32: signed offset from the instruction address in the
    // word-1 slot.  A resolved Value is target − (instr+4) (the fixup is at
    // offset 4); unresolved, the relocation carries it and Value is 0.
    case KlaussCPU::FK_KlaussCPU_PCREL32:
      if (IsResolved) {
        int64_t Disp = static_cast<int64_t>(Value) + 4;
        if (!isInt<32>(Disp)) {
          getContext().reportError(Fixup.getLoc(),
                                   "PC-relative target out of range");
          return;
        }
        Value = static_cast<uint64_t>(Disp);
      }
      NumBytes = 4;
      break;
    default:
      getContext().reportError(Fixup.getLoc(), "unsupported fixup kind");
      return;
    }
    if (NumBytes < 8 && !isUIntN(NumBytes * 8, Value) &&
        !isIntN(NumBytes * 8, Value)) {
      getContext().reportError(Fixup.getLoc(),
                               "fixup value out of range for " +
                                   Twine(NumBytes) + "-byte field");
      return;
    }
    // Little-endian, like the data bus and the instruction words.
    for (unsigned I = 0; I != NumBytes; ++I)
      Data[I] = static_cast<uint8_t>(Value >> (8 * I));
  }

  // ── ISA v3 A1 short-branch relaxation ─────────────────────────────────────

  // The short branches are the instructions whose CompressPat expands back
  // (KlaussCPUInstrInfo.td); relaxing one is that expansion.  Only a symbolic
  // operand has a fixup that can end up out of range, so an instruction
  // without one is final whatever its patterns say.
  bool mayNeedRelaxation(unsigned Opcode, ArrayRef<MCOperand> Operands,
                         const MCSubtargetInfo &STI) const override {
    if (none_of(Operands, [](const MCOperand &Op) { return Op.isExpr(); }))
      return false;
    MCInst Inst, Long;
    Inst.setOpcode(Opcode);
    for (const MCOperand &Op : Operands)
      Inst.addOperand(Op);
    return KlaussCPU::uncompress(Long, Inst, STI);
  }

  // Stay short only when the target is resolved (same section, local) and
  // the word displacement fits simm18.
  bool fixupNeedsRelaxationAdvanced(const MCFragment &, const MCFixup &Fixup,
                                    const MCValue &, uint64_t Value,
                                    bool Resolved) const override {
    if (Fixup.getKind() != MCFixupKind(KlaussCPU::FK_KlaussCPU_PCREL18))
      return !Resolved;
    if (!Resolved)
      return true;
    int64_t V = static_cast<int64_t>(Value);
    return (V & 3) != 0 || !isInt<18>(V >> 2);
  }

  void relaxInstruction(MCInst &Inst,
                        const MCSubtargetInfo &STI) const override {
    MCInst Long;
    [[maybe_unused]] bool Success = KlaussCPU::uncompress(Long, Inst, STI);
    assert(Success && "relaxing a non-short-branch");
    Inst = std::move(Long);
  }

  // ── NOP padding ──────────────────────────────────────────────────────────

  // v2 NOP = 0x6C000000 (4-byte, little-endian: 00 00 00 6C).
  // Padding that is not a multiple of 4 follows data (.byte, .ascii) in a code
  // section, so its first Count % 4 bytes can never be executed: zero-fill
  // them and NOP-fill from the next word boundary, as RISC-V does.
  bool writeNopData(raw_ostream &OS, uint64_t Count,
                    const MCSubtargetInfo *STI) const override {
    OS.write_zeros(Count % 4);
    for (uint64_t i = 0, n = Count / 4; i < n; ++i)
      OS.write("\x00\x00\x00\x6C", 4);
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
