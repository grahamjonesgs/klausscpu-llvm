//===-- KlaussCPUAsmPrinter.cpp - KlaussCPU LLVM assembly writer -----------===//
//
// KlaussCPU LLVM backend — converts MachineInstr to text assembly via MCInst.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUTargetMachine.h"
#include "MCTargetDesc/KlaussCPUInstPrinter.h"
#include "TargetInfo/KlaussCPUTargetInfo.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define DEBUG_TYPE "asm-printer"

namespace {

class KlaussCPUAsmPrinter : public AsmPrinter {
public:
  explicit KlaussCPUAsmPrinter(TargetMachine &TM,
                                std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer), ID) {}

  StringRef getPassName() const override {
    return "KlaussCPU Assembly Printer";
  }

  void emitInstruction(const MachineInstr *MI) override;

  static char ID;
};

} // end anonymous namespace

char KlaussCPUAsmPrinter::ID = 0;

// Lower a single MachineOperand to an MCOperand.
// Returns an invalid MCOperand for implicit register operands (caller skips them).
static MCOperand lowerMachineOperand(const MachineOperand &MO,
                                      AsmPrinter &AP) {
  switch (MO.getType()) {
  case MachineOperand::MO_Register:
    if (MO.isImplicit())
      return MCOperand(); // invalid — caller skips
    return MCOperand::createReg(MO.getReg());

  case MachineOperand::MO_Immediate:
    return MCOperand::createImm(MO.getImm());

  case MachineOperand::MO_MachineBasicBlock:
    return MCOperand::createExpr(
        MCSymbolRefExpr::create(MO.getMBB()->getSymbol(), AP.OutContext));

  case MachineOperand::MO_GlobalAddress:
    return MCOperand::createExpr(
        MCSymbolRefExpr::create(AP.getSymbol(MO.getGlobal()), AP.OutContext));

  case MachineOperand::MO_ExternalSymbol:
    return MCOperand::createExpr(MCSymbolRefExpr::create(
        AP.GetExternalSymbolSymbol(MO.getSymbolName()), AP.OutContext));

  case MachineOperand::MO_JumpTableIndex:
    return MCOperand::createExpr(MCSymbolRefExpr::create(
        AP.GetJTISymbol(MO.getIndex()), AP.OutContext));

  default:
    llvm_unreachable("KlaussCPU AsmPrinter: unhandled operand type");
  }
}

void KlaussCPUAsmPrinter::emitInstruction(const MachineInstr *MI) {
  MCInst Inst;
  Inst.setOpcode(MI->getOpcode());

  for (const MachineOperand &MO : MI->explicit_operands()) {
    MCOperand Op = lowerMachineOperand(MO, *this);
    if (Op.isValid())
      Inst.addOperand(Op);
  }

  EmitToStreamer(*OutStreamer, Inst);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeKlaussCPUAsmPrinter() {
  RegisterAsmPrinter<KlaussCPUAsmPrinter> X(getTheKlaussCPUTarget());
}
