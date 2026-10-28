//===-- KlaussCPUCalleeSavedCFI.cpp - Late CFI for callee-saved spills ----===//
//
// Places the .cfi_offset rule of each callee-saved register directly behind
// the store that spills it, after the post-RA scheduler has run.
//
// A CFI instruction is a scheduling boundary (TargetInstrInfo::
// isSchedulingBoundary: isPosition).  ENTER/LEAVE and the other prologue and
// epilogue instructions that carry the rest of the frame's CFI write SP, so
// they are boundaries anyway and their CFI changes nothing.  The spills are
// plain stores, though: a block of .cfi_offset rules behind them, built by
// emitPrologue only with -g or unwind tables, would keep the scheduler from
// moving body instructions across them, so -g would change the code (the
// entry block's spills could no longer fill the bubble behind ENTER's R15).
// Built here instead, the rules never meet the scheduler, and each one sits
// directly behind its own spill, which is also exact: until then the
// register still holds the caller's value, and nothing writes it before its
// spill (the write would depend on the spill).
//
// Spill slots are R15-relative (getFrameIndexReference) and R15 = CFA-16.
// The spills are the FrameSetup stores of the prologue block
// (spillCalleeSavedRegisters): the entry block, or the save point that
// shrink-wrapping chose.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUFrameLowering.h"
#include "KlaussCPUInstrInfo.h"
#include "KlaussCPUSubtarget.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/CodeGen/CFIInstBuilder.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"

using namespace llvm;

#define DEBUG_TYPE "klausscpu-csr-cfi"
#define PASS_NAME "KlaussCPU callee-saved register CFI"

namespace llvm {
void initializeKlaussCPUCalleeSavedCFIPass(PassRegistry &);
FunctionPass *createKlaussCPUCalleeSavedCFIPass();
} // namespace llvm

namespace {

class KlaussCPUCalleeSavedCFI : public MachineFunctionPass {
public:
  static char ID;
  KlaussCPUCalleeSavedCFI() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return PASS_NAME; }

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::NoVRegs);
  }
};

} // end anonymous namespace

char KlaussCPUCalleeSavedCFI::ID = 0;

INITIALIZE_PASS(KlaussCPUCalleeSavedCFI, DEBUG_TYPE, PASS_NAME, false, false)

bool KlaussCPUCalleeSavedCFI::runOnMachineFunction(MachineFunction &MF) {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  if (!MF.needsFrameMoves() || MFI.getCalleeSavedInfo().empty())
    return false;

  const auto &TFI = *static_cast<const KlaussCPUFrameLowering *>(
      MF.getSubtarget().getFrameLowering());
  DenseMap<MCRegister, int64_t> CFAOffset;
  for (const CalleeSavedInfo &CS : MFI.getCalleeSavedInfo()) {
    Register FrameReg;
    StackOffset Offset =
        TFI.getFrameIndexReference(MF, CS.getFrameIdx(), FrameReg);
    assert(FrameReg == KlaussCPU::R15 && "Callee-saved slot not R15-relative");
    CFAOffset[CS.getReg()] = Offset.getFixed() - 16;
  }

  // PEI has dropped the save point by now; the spills are the only
  // FrameSetup stores of a callee-saved register.
  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : MBB) {
      if (!MI.getFlag(MachineInstr::FrameSetup) || !MI.mayStore() ||
          !MI.getOperand(0).isReg())
        continue;
      auto It = CFAOffset.find(MI.getOperand(0).getReg().asMCReg());
      if (It == CFAOffset.end())
        continue;
      CFIInstBuilder(MBB, std::next(MI.getIterator()),
                     MachineInstr::FrameSetup)
          .buildOffset(It->first, It->second);
      CFAOffset.erase(It);
    }
  }
  assert(CFAOffset.empty() && "Callee-saved register without a spill");
  return true;
}

FunctionPass *llvm::createKlaussCPUCalleeSavedCFIPass() {
  return new KlaussCPUCalleeSavedCFI();
}
