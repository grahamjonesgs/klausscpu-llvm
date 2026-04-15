//===-- KlaussCPUInstPrinter.cpp - Convert KlaussCPU MCInst to asm text ----===//
//
// KlaussCPU LLVM backend — MCInstPrinter implementation.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUInstPrinter.h"
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
    O << Op.getImm();
    return;
  }
  assert(Op.isExpr() && "expected expression operand");
  MAI.printExpr(O, *Op.getExpr());
}
