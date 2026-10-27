//===-- KlaussCPUInstPrinter.cpp - Convert KlaussCPU MCInst to asm text ----===//
//
// KlaussCPU LLVM backend — MCInstPrinter implementation.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"

#define GET_INSTRINFO_ENUM
#include "KlaussCPUGenInstrInfo.inc"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "asm-printer"

// Include the auto-generated asm writer tables.
#include "KlaussCPUGenAsmWriter.inc"

void KlaussCPUInstPrinter::printRegName(raw_ostream &OS, MCRegister Reg) {
  OS << getRegisterName(Reg);
}

void KlaussCPUInstPrinter::printInst(const MCInst *MI, uint64_t Address,
                                      StringRef Annot,
                                      const MCSubtargetInfo &STI,
                                      raw_ostream &O) {
  printInstruction(MI, Address, STI, O);
  printAnnotation(O, Annot);
}

void KlaussCPUInstPrinter::printOperand(const MCInst *MI, unsigned OpNo,
                                         const MCSubtargetInfo & /*STI*/,
                                         raw_ostream &O) {
  const MCOperand &Op = MI->getOperand(OpNo);
  if (Op.isReg()) {
    printRegName(O, Op.getReg());
    return;
  }
  if (Op.isImm()) {
    // An absolute JMP/CALL target holds the address; with
    // PrintBranchImmAsAddress (llvm-objdump) it prints as one.
    if (PrintBranchImmAsAddress && OpNo == 0 &&
        (MII.get(MI->getOpcode()).isBranch() ||
         MII.get(MI->getOpcode()).isCall())) {
      O << formatHex(uint64_t(uint32_t(Op.getImm())));
      return;
    }
    O << Op.getImm();
    return;
  }
  assert(Op.isExpr() && "expected expression operand");
  MAI.printExpr(O, *Op.getExpr());
}

void KlaussCPUInstPrinter::printOperand(const MCInst *MI, uint64_t Address,
                                         unsigned OpNo,
                                         const MCSubtargetInfo &STI,
                                         raw_ostream &O) {
  const MCOperand &Op = MI->getOperand(OpNo);
  if (!Op.isImm())
    return printOperand(MI, OpNo, STI, O);
  // The byte displacement from this instruction; with PrintBranchImmAsAddress
  // (llvm-objdump) it prints as the target address.  JMPO/JMPNO have no
  // PC-relative spelling, so their short forms print with the absolute
  // mnemonic and must always show the target address.
  uint64_t Target = uint32_t(Address + Op.getImm());
  if (PrintBranchImmAsAddress)
    O << formatHex(Target);
  else if (MI->getOpcode() == KlaussCPU::JMPO_SH ||
           MI->getOpcode() == KlaussCPU::JMPNO_SH)
    O << Target;
  else
    O << Op.getImm();
}
