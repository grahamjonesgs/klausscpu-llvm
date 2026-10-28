//===-- KlaussCPULeaveRet.cpp - Fold LEAVE + RET into LEAVERET ------------===//
//
// emitEpilogue folds the epilogue's LEAVE into a plain return as LEAVERET
// (one instruction, one memory access when [R15+8] is in R15's cache line)
// when the epilogue is in the return block.  A shrink-wrapped epilogue sits at
// the restore point instead, a block that falls through or branches to the
// return block, so it ends in a LEAVE:
//
//     leave                      leave
//     ret                        jmp  .Lret   ->  .Lret: ret
//
// The first is what tail duplication makes of the second, sometimes with the
// return block's own instructions behind the LEAVE.  When the return block
// is shared with a frameless path, it may do a little more than return
// (`addiw r12, r12, 1 ; ret`).  This pre-emit pass, after block placement,
// turns all of them into LEAVERET at the return, moving the LEAVE past
// instructions that do not use R15 or SP and copying up to two of the return
// block's.  The epilogue's CFI
// between LEAVE and the return goes too: LEAVERET leaves the function in the
// same instruction, as in emitEpilogue (KlaussCPUFrameLowering.cpp), and the
// CFIFixup pass gives a block laid out after it the state it is entered with.
// The CFI and debug instructions are skipped when matching, so -g changes
// nothing.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUFrameLowering.h"
#include "KlaussCPUInstrInfo.h"
#include "KlaussCPUSubtarget.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

using namespace llvm;

#define DEBUG_TYPE "klausscpu-fold-leaveret"
#define PASS_NAME "KlaussCPU LEAVE + RET to LEAVERET"

namespace llvm {
void initializeKlaussCPULeaveRetPass(PassRegistry &);
FunctionPass *createKlaussCPULeaveRetPass();
} // namespace llvm

namespace {

class KlaussCPULeaveRet : public MachineFunctionPass {
public:
  static char ID;
  KlaussCPULeaveRet() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return PASS_NAME; }

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::NoVRegs);
  }
};

} // end anonymous namespace

char KlaussCPULeaveRet::ID = 0;

INITIALIZE_PASS(KlaussCPULeaveRet, DEBUG_TYPE, PASS_NAME, false, false)

// Skip the CFI and debug instructions from I on.
static MachineBasicBlock::iterator skipMeta(MachineBasicBlock::iterator I,
                                            MachineBasicBlock::iterator E) {
  while (I != E && (I->isCFIInstruction() || I->isDebugInstr()))
    ++I;
  return I;
}

// Can LEAVE move down past MI?  MI must not use the frame registers, and
// nothing whose effect is not modelled may be in the way.  Keeping the frame
// a little longer is harmless: the code after a restore point uses nothing in
// it.
static bool canSinkLeavePast(const MachineInstr &MI) {
  return !MI.isCall() && !MI.isInlineAsm() && !MI.hasUnmodeledSideEffects() &&
         !MI.readsRegister(KlaussCPU::R15, /*TRI=*/nullptr) &&
         !MI.modifiesRegister(KlaussCPU::R15, /*TRI=*/nullptr) &&
         !MI.readsRegister(KlaussCPU::SP, /*TRI=*/nullptr) &&
         !MI.modifiesRegister(KlaussCPU::SP, /*TRI=*/nullptr);
}

// The return block's own instructions that may be copied in front of the
// LEAVERET (at most MaxRetBlockCopies, which cost their size; the fold saves
// an instruction and a memory access on every pass), and its RET.
static constexpr unsigned MaxRetBlockCopies = 2;
static MachineInstr *returnBlock(MachineBasicBlock &RetBB,
                                 SmallVectorImpl<MachineInstr *> &Copies) {
  for (MachineInstr &MI : RetBB) {
    if (MI.isCFIInstruction() || MI.isDebugInstr())
      continue;
    if (MI.getOpcode() == KlaussCPU::RET_I)
      return &MI;
    if (MI.isTerminator() || MI.isNotDuplicable() || !canSinkLeavePast(MI) ||
        Copies.size() == MaxRetBlockCopies)
      return nullptr;
    Copies.push_back(&MI);
  }
  return nullptr;
}

static bool foldLeaveRet(MachineBasicBlock &MBB,
                         const KlaussCPUInstrInfo &TII) {
  // How MBB leaves: its RET, or a fall-through or a jump to a block that
  // returns after at most MaxRetBlockCopies instructions (shared with the
  // frameless path, so tail duplication left them there), which are copied.
  MachineBasicBlock::iterator Exit = MBB.getFirstTerminator();
  MachineInstr *Ret = nullptr;
  MachineBasicBlock *RetBB = nullptr;
  SmallVector<MachineInstr *, MaxRetBlockCopies> Copies;
  if (Exit != MBB.end() && Exit->getOpcode() == KlaussCPU::RET_I) {
    Ret = &*Exit;
  } else if (MBB.succ_size() == 1 &&
             (Exit == MBB.end() ||
              (Exit->isUnconditionalBranch() && !Exit->isIndirectBranch() &&
               skipMeta(std::next(Exit), MBB.end()) == MBB.end()))) {
    RetBB = *MBB.succ_begin();
    Ret = returnBlock(*RetBB, Copies);
  }
  if (!Ret)
    return false;

  // The LEAVE in front of it, with nothing in between that needs the frame
  // gone (tail duplication can put the return block's own instructions
  // there: `leave ; addiw r12, r12, 1 ; ret`).
  MachineBasicBlock::iterator Leave = Exit;
  while (true) {
    if (Leave == MBB.begin())
      return false;
    --Leave;
    if (Leave->isCFIInstruction() || Leave->isDebugInstr())
      continue;
    if (Leave->getOpcode() == KlaussCPU::LEAVE_I)
      break;
    if (!canSinkLeavePast(*Leave))
      return false;
  }

  // Keep RET's implicit uses (the return-value registers), as emitEpilogue
  // does.  The epilogue's CFI goes with the LEAVE.
  MachineFunction &MF = *MBB.getParent();
  for (MachineInstr *MI : Copies)
    MBB.insert(MBB.end(), MF.CloneMachineInstr(MI));
  BuildMI(MBB, MBB.end(), Leave->getDebugLoc(), TII.get(KlaussCPU::LEAVERET_I))
      .copyImplicitOps(*Ret)
      .setMIFlag(MachineInstr::FrameDestroy);
  for (MachineInstr &MI : make_early_inc_range(make_range(Leave, Exit)))
    if (&MI == &*Leave || (MI.isCFIInstruction() &&
                           MI.getFlag(MachineInstr::FrameDestroy)))
      MI.eraseFromParent();
  if (Exit != MBB.end()) {
    // The RET or the jump, and anything (debug instructions) behind it, up
    // to the copies.
    MachineBasicBlock::iterator End =
        Copies.empty() ? std::prev(MBB.end())
                       : std::prev(MBB.end(), Copies.size() + 1);
    while (Exit != End)
      (Exit++)->eraseFromParent();
  }
  if (RetBB) {
    MBB.removeSuccessor(RetBB);
    // A return block that was only reached from here is dead now.
    if (RetBB->pred_empty() && !RetBB->hasAddressTaken() &&
        !RetBB->isEHPad() && RetBB != &MBB.getParent()->front())
      RetBB->eraseFromParent();
  }
  return true;
}

bool KlaussCPULeaveRet::runOnMachineFunction(MachineFunction &MF) {
  if (!KlaussCPUFrameLowering::useLeaveRet())
    return false;
  const KlaussCPUInstrInfo &TII =
      *MF.getSubtarget<KlaussCPUSubtarget>().getInstrInfo();
  bool Changed = false;
  for (MachineBasicBlock &MBB : MF)
    Changed |= foldLeaveRet(MBB, TII);
  return Changed;
}

FunctionPass *llvm::createKlaussCPULeaveRetPass() {
  return new KlaussCPULeaveRet();
}
