//===-- KlaussCPUInstPrinter.cpp - Convert KlaussCPU MCInst to asm text ----===//
//
// KlaussCPU LLVM backend — MCInstPrinter implementation.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUInstPrinter.h"
#include "KlaussCPUMCTargetDesc.h"
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
                                      const MCSubtargetInfo & /*STI*/,
                                      raw_ostream &O) {
  CurAddress = Address;
  printInstruction(MI, Address, O);
  printAnnotation(O, Annot);
}

void KlaussCPUInstPrinter::printOperand(const MCInst *MI, unsigned OpNo,
                                         raw_ostream &O) {
  const MCOperand &Op = MI->getOperand(OpNo);
  if (Op.isReg()) {
    printRegName(O, Op.getReg());
    return;
  }
  if (Op.isImm()) {
    // Branch targets: PC-relative operands hold the byte displacement from
    // this instruction; absolute JMP/CALL targets hold the address.  With
    // PrintBranchImmAsAddress (llvm-objdump) both print as the target address.
    // JMPO/JMPNO have no PC-relative spelling, so their short forms print
    // with the absolute mnemonic and must always show the target address.
    if ((MI->getOpcode() == KlaussCPU::JMPO_SH ||
         MI->getOpcode() == KlaussCPU::JMPNO_SH) && OpNo == 0) {
      uint64_t Target = uint32_t(CurAddress + Op.getImm());
      if (PrintBranchImmAsAddress)
        O << formatHex(Target);
      else
        O << Target;
      return;
    }
    if (PrintBranchImmAsAddress) {
      int PCRelOp = getKlaussCPUPCRelOperand(MI->getOpcode());
      const MCInstrDesc &Desc = MII.get(MI->getOpcode());
      if (int(OpNo) == PCRelOp) {
        O << formatHex(uint64_t(uint32_t(CurAddress + Op.getImm())));
        return;
      }
      if (PCRelOp < 0 && OpNo == 0 && (Desc.isBranch() || Desc.isCall())) {
        O << formatHex(uint64_t(uint32_t(Op.getImm())));
        return;
      }
    }
    O << Op.getImm();
    return;
  }
  assert(Op.isExpr() && "expected expression operand");
  MAI.printExpr(O, *Op.getExpr());
}
