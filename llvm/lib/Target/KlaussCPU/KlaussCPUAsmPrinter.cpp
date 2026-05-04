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
#include "llvm/MC/MCExpr.h"
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

  // Inline asm operand substitution: called for each $N in an asm string.
  bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                       const char *ExtraCode, raw_ostream &OS) override;

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

  case MachineOperand::MO_GlobalAddress: {
    const MCExpr *Sym =
        MCSymbolRefExpr::create(AP.getSymbol(MO.getGlobal()), AP.OutContext);
    if (int64_t Off = MO.getOffset())
      Sym = MCBinaryExpr::createAdd(
          Sym, MCConstantExpr::create(Off, AP.OutContext), AP.OutContext);
    return MCOperand::createExpr(Sym);
  }

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

bool KlaussCPUAsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                                           const char *ExtraCode,
                                           raw_ostream &OS) {
  // Delegate modifier letters ('c', 'n', 'a', 's') to the base class.
  if (ExtraCode && ExtraCode[0])
    return AsmPrinter::PrintAsmOperand(MI, OpNo, ExtraCode, OS);

  // No modifier: print the raw register name or immediate value.
  const MachineOperand &MO = MI->getOperand(OpNo);
  switch (MO.getType()) {
  case MachineOperand::MO_Register:
    OS << KlaussCPUInstPrinter::getRegisterName(MO.getReg());
    return false;
  case MachineOperand::MO_Immediate:
    OS << MO.getImm();
    return false;
  default:
    return true;
  }
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeKlaussCPUAsmPrinter() {
  RegisterAsmPrinter<KlaussCPUAsmPrinter> X(getTheKlaussCPUTarget());
}
