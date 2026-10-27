//===-- KlaussCPUMCCodeEmitter.cpp - KlaussCPU instruction encoder ---------===//
//
// Converts MCInst to raw bytes for ELF object output.  The encodings are the
// `Inst` fields of KlaussCPUInstrInfo.td (ISA v2 + v3); this file only
// supplies the operand hooks TableGen has no mechanism for (register numbers
// and the fixups of symbolic operands) and writes the instruction out.
//
// Instructions are 1, 2 or 3 32-bit words; word N is Inst{32*N+31 - 32*N}.
// The widest (SETR64, PUSHV64) is 96 bits, so the generated
// getBinaryCodeForInstr works on an APInt.  All words are written
// little-endian (least-significant byte first), matching the KlaussCPU LE
// physical memory / instruction-fetch unit.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUFixupKinds.h"
#include "KlaussCPUMCTargetDesc.h"
#include "llvm/ADT/APInt.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"

#define GET_INSTRINFO_ENUM
#include "KlaussCPUGenInstrInfo.inc"

using namespace llvm;

namespace {

class KlaussCPUMCCodeEmitter : public MCCodeEmitter {
  const MCInstrInfo &MCII;
  MCContext &Ctx;

public:
  KlaussCPUMCCodeEmitter(const MCInstrInfo &MCII, MCContext &Ctx)
      : MCII(MCII), Ctx(Ctx) {}

  void encodeInstruction(const MCInst &MI, SmallVectorImpl<char> &CB,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const override;

  // TableGen'erated function (KlaussCPUGenMCCodeEmitter.inc).
  void getBinaryCodeForInstr(const MCInst &MI,
                             SmallVectorImpl<MCFixup> &Fixups, APInt &Inst,
                             APInt &Scratch,
                             const MCSubtargetInfo &STI) const;

  // Register number or constant of an operand without an EncoderMethod.
  void getMachineOpValue(const MCInst &MI, const MCOperand &MO, APInt &Op,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const;

  // EncoderMethods of the operands that may be a symbol: a constant encodes
  // as itself (a PC-relative one is the byte displacement, which the .td
  // field slices), an expression as 0 plus a fixup on its field.
  void getAbs32OpValue(const MCInst &MI, unsigned OpNo, APInt &Op,
                       SmallVectorImpl<MCFixup> &Fixups,
                       const MCSubtargetInfo &STI) const {
    getImmOrFixup(MI, OpNo, Op, Fixups, 4, KlaussCPU::FK_KlaussCPU_ABS32,
                  /*PCRel=*/false);
  }
  void getPCRel32OpValue(const MCInst &MI, unsigned OpNo, APInt &Op,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const {
    getImmOrFixup(MI, OpNo, Op, Fixups, 4, KlaussCPU::FK_KlaussCPU_PCREL32,
                  /*PCRel=*/true);
  }
  void getPCRel18OpValue(const MCInst &MI, unsigned OpNo, APInt &Op,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const {
    getImmOrFixup(MI, OpNo, Op, Fixups, 0, KlaussCPU::FK_KlaussCPU_PCREL18,
                  /*PCRel=*/true);
  }
  void getPCRel13OpValue(const MCInst &MI, unsigned OpNo, APInt &Op,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const {
    getImmOrFixup(MI, OpNo, Op, Fixups, 0, KlaussCPU::FK_KlaussCPU_PCREL13,
                  /*PCRel=*/true);
  }

private:
  // Offset is the byte offset of the fixup within the instruction: 4 for a
  // word-1 field, 0 for a field in word 0.
  void getImmOrFixup(const MCInst &MI, unsigned OpNo, APInt &Op,
                     SmallVectorImpl<MCFixup> &Fixups, unsigned Offset,
                     KlaussCPU::Fixups Kind, bool PCRel) const;
};

} // anonymous namespace

void KlaussCPUMCCodeEmitter::getMachineOpValue(
    const MCInst &MI, const MCOperand &MO, APInt &Op,
    SmallVectorImpl<MCFixup> &Fixups, const MCSubtargetInfo &STI) const {
  if (MO.isReg()) {
    Op = Ctx.getRegisterInfo()->getEncodingValue(MO.getReg());
    return;
  }
  if (MO.isImm()) {
    Op = static_cast<uint64_t>(MO.getImm());
    return;
  }
  // An operand with no EncoderMethod has no fixup to resolve a symbol with.
  // The parser accepts any expression in an immediate slot, so diagnose it
  // here rather than encode a wrong value.
  assert(MO.isExpr() && "operand must be reg, imm or expr");
  Ctx.reportError(MI.getLoc(), "operand must be a constant: no relocation "
                               "exists for a symbol in this field");
  Op = 0;
}

void KlaussCPUMCCodeEmitter::getImmOrFixup(const MCInst &MI, unsigned OpNo,
                                           APInt &Op,
                                           SmallVectorImpl<MCFixup> &Fixups,
                                           unsigned Offset,
                                           KlaussCPU::Fixups Kind,
                                           bool PCRel) const {
  const MCOperand &MO = MI.getOperand(OpNo);
  if (MO.isImm()) {
    Op = static_cast<uint64_t>(MO.getImm());
    return;
  }
  assert(MO.isExpr() && "operand must be imm or expr");
  Fixups.push_back(
      MCFixup::create(Offset, MO.getExpr(), MCFixupKind(Kind), PCRel));
  Op = 0;
}

void KlaussCPUMCCodeEmitter::encodeInstruction(
    const MCInst &MI, SmallVectorImpl<char> &CB,
    SmallVectorImpl<MCFixup> &Fixups, const MCSubtargetInfo &STI) const {
  APInt Bits, Scratch;
  getBinaryCodeForInstr(MI, Fixups, Bits, Scratch, STI);
  unsigned Size = MCII.get(MI.getOpcode()).getSize();
  for (unsigned I = 0; I != Size; ++I)
    CB.push_back(static_cast<char>(Bits.extractBitsAsZExtValue(8, 8 * I)));
}

#include "KlaussCPUGenMCCodeEmitter.inc"

//===----------------------------------------------------------------------===//
// Factory
//===----------------------------------------------------------------------===//

namespace llvm {
MCCodeEmitter *createKlaussCPUMCCodeEmitter(const MCInstrInfo &MCII,
                                             MCContext &Ctx) {
  return new KlaussCPUMCCodeEmitter(MCII, Ctx);
}
} // namespace llvm
