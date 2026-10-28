//===-- KlaussCPUAsmPrinter.cpp - KlaussCPU LLVM assembly writer -----------===//
//
// KlaussCPU LLVM backend — converts MachineInstr to text assembly via MCInst.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUTargetMachine.h"
#include "MCTargetDesc/KlaussCPUInstPrinter.h"
#include "MCTargetDesc/KlaussCPUMCTargetDesc.h"
#include "TargetInfo/KlaussCPUTargetInfo.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/IR/InlineAsm.h"
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
  bool PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNo,
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

  case MachineOperand::MO_BlockAddress: {
    // blockaddress(@f, %bb) — GNU computed goto (`&&label`).
    const MCExpr *Sym = MCSymbolRefExpr::create(
        AP.GetBlockAddressSymbol(MO.getBlockAddress()), AP.OutContext);
    if (int64_t Off = MO.getOffset())
      Sym = MCBinaryExpr::createAdd(
          Sym, MCConstantExpr::create(Off, AP.OutContext), AP.OutContext);
    return MCOperand::createExpr(Sym);
  }

  case MachineOperand::MO_JumpTableIndex:
    return MCOperand::createExpr(MCSymbolRefExpr::create(
        AP.GetJTISymbol(MO.getIndex()), AP.OutContext));

  default:
    llvm_unreachable("KlaussCPU AsmPrinter: unhandled operand type");
  }
}

void KlaussCPUAsmPrinter::emitInstruction(const MachineInstr *MI) {
  KlaussCPU_MC::verifyInstructionPredicates(
      MI->getOpcode(), getSubtargetInfo().getFeatureBits());

  MCInst Inst;
  switch (MI->getOpcode()) {
  case KlaussCPU::PseudoTAIL:
    Inst.setOpcode(KlaussCPU::TAILREL);
    break;
  case KlaussCPU::PseudoTAILIndirect:
    Inst.setOpcode(KlaussCPU::JMPR_R);
    break;
  default:
    Inst.setOpcode(MI->getOpcode());
  }

  for (const MachineOperand &MO : MI->explicit_operands()) {
    MCOperand Op = lowerMachineOperand(MO, *this);
    if (Op.isValid())
      Inst.addOperand(Op);
  }

  // ISA v3 short 1-word forms.
  MCInst CInst;
  bool Compressed = KlaussCPU::compress(CInst, Inst, getSubtargetInfo());
  EmitToStreamer(*OutStreamer, Compressed ? CInst : Inst);
}

bool KlaussCPUAsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                                           const char *ExtraCode,
                                           raw_ostream &OS) {
  // Delegate modifier letters ('c', 'n', 'a', 's') to the base class.
  if (ExtraCode && ExtraCode[0])
    return AsmPrinter::PrintAsmOperand(MI, OpNo, ExtraCode, OS);

  // No modifier: print the register name, immediate value or symbol.
  const MachineOperand &MO = MI->getOperand(OpNo);
  switch (MO.getType()) {
  case MachineOperand::MO_Register:
    OS << KlaussCPUInstPrinter::getRegisterName(MO.getReg());
    return false;
  case MachineOperand::MO_Immediate:
    OS << MO.getImm();
    return false;
  case MachineOperand::MO_GlobalAddress:
    PrintSymbolOperand(MO, OS);
    return false;
  case MachineOperand::MO_ExternalSymbol:
    GetExternalSymbolSymbol(MO.getSymbolName())->print(OS, MAI);
    printOffset(MO.getOffset(), OS);
    return false;
  case MachineOperand::MO_BlockAddress:
    GetBlockAddressSymbol(MO.getBlockAddress())->print(OS, MAI);
    printOffset(MO.getOffset(), OS);
    return false;
  default:
    return true;
  }
}

// Inline asm memory operand ('m', 'o'): the (base register, offset) pair from
// KlaussCPUDAGToDAGISel::SelectInlineAsmMemoryOperand, printed as
// `base, offset` so that `ldidx64 $0, $1` expands to `ldidx64 rd, base, off`.
bool KlaussCPUAsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI,
                                                 unsigned OpNo,
                                                 const char *ExtraCode,
                                                 raw_ostream &OS) {
  if (ExtraCode)
    return AsmPrinter::PrintAsmMemoryOperand(MI, OpNo, ExtraCode, OS);

  const MachineOperand &Base = MI->getOperand(OpNo);
  if (!Base.isReg())
    return true;

  // The generic '%a' modifier on a register ('r') operand lands here too.
  // That operand is a lone register with no offset after it (OpNo + 1 is the
  // next operand's flag word, or past the end), so address it with offset 0.
  const MachineOperand &FlagMO = MI->getOperand(OpNo - 1);
  if (!FlagMO.isImm() || !InlineAsm::Flag(FlagMO.getImm()).isMemKind()) {
    OS << KlaussCPUInstPrinter::getRegisterName(Base.getReg()) << ", 0";
    return false;
  }

  assert(MI->getNumOperands() > OpNo + 1 && "Expected offset operand");
  const MachineOperand &Offset = MI->getOperand(OpNo + 1);
  if (!Offset.isImm())
    return true;
  OS << KlaussCPUInstPrinter::getRegisterName(Base.getReg()) << ", "
     << Offset.getImm();
  return false;
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeKlaussCPUAsmPrinter() {
  RegisterAsmPrinter<KlaussCPUAsmPrinter> X(getTheKlaussCPUTarget());
}
