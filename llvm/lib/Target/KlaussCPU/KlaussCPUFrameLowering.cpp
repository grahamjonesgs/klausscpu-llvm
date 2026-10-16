//===-- KlaussCPUFrameLowering.cpp - KlaussCPU Frame Lowering -------------===//
//
// KlaussCPU LLVM backend — frame lowering implementation.
//
// Frame layout (stack grows down):
//
//   High addresses (caller)
//   ┌──────────────┐  ← CallerSP  (before call)
//   │  Stack args  │    [CallerSP+32..CallerSP+32+N*8]
//   ├──────────────┤  ← [CallerSP+32]  (first stack arg)
//   │   (reserved) │    [CallerSP+8..CallerSP+31]
//   ├──────────────┤  ← [CallerSP+8]
//   │  Return addr │    [CallerSP+0..CallerSP+7]  (saved by CALL instruction)
//   ├──────────────┤  ← CalleeSP_on_entry  (= CallerSP after CALL)
//   │   Saved R15  │  ← PUSH R15 decrements SP, then stores R15
//   ├──────────────┤  ← R15 = GETSP after push (frame pointer)
//   │   Locals     │    [R15 - MFI.getStackSize() .. R15 - 8]
//   └──────────────┤  ← SP after ADDSP -N (bottom of frame)
//
// Prologue:  PUSH R15      ; save caller's frame pointer
//            GETSP R15     ; R15 ← new frame base
//            ADDSP -N      ; allocate N bytes of locals  (omitted if N == 0)
//
// Epilogue:  SETSP R15     ; restore SP to frame base  (discards locals)
//            POP R15       ; restore caller's R15
//            (RET is emitted by the KlaussCPURetGlue pattern)
//
// Call frame information (emitted when MF.needsFrameMoves(), i.e. with -g or
// unwind tables).  CFA = the caller's SP before its CALL.  The CIE (see
// createKlaussCPUMCAsmInfo) starts each function at CFA = SP+8 with the return
// address (RA column, DWARF 17) at CFA-8.  After the prologue CFA = R15+16
// with the caller's R15 at CFA-16; callee-saved spills add .cfi_offset rules.
// SP moves after the prologue (VLAs, call frames) need no CFI because the CFA
// is R15-based.  An epilogue folded into LEAVERET leaves the function in the
// same instruction, so it needs no CFI; a plain LEAVE switches the CFA back to
// SP+8 and the CFIFixup pass restores the frame state for any blocks laid out
// after that epilogue.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUFrameLowering.h"
#include "KlaussCPUInstrInfo.h"
#include "KlaussCPUSubtarget.h"
#include "llvm/CodeGen/CFIInstBuilder.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

// ISA v3 C: one-word ENTER/LEAVE instead of PUSH R15; GETSP R15; ADDSP -N /
// SETSP R15; POP R15.
static cl::opt<bool> EnableLeaveRet(
    "klausscpu-leaveret", cl::init(true), cl::Hidden,
    cl::desc("Fold the epilogue LEAVE + RET into LEAVERET"));
static cl::opt<bool> EnableEnterLeave(
    "klausscpu-enter-leave", cl::init(true), cl::Hidden,
    cl::desc("Use the ISA v3 ENTER/LEAVE frame instructions"));

// ENTER encodes the frame size in 8-byte units in a 22-bit field.
static bool fitsEnter(uint64_t FrameSize) {
  return FrameSize % 8 == 0 && (FrameSize / 8) < (uint64_t(1) << 22);
}

bool KlaussCPUFrameLowering::hasReservedCallFrame(
    const MachineFunction &MF) const {
  // Reserve space for outgoing call frames in the prologue so that
  // CALLSEQ_START/END pseudos are eliminated as no-ops.  This is safe as long
  // as there are no dynamically-sized objects (VLAs / alloca with runtime size).
  return !MF.getFrameInfo().hasVarSizedObjects();
}

MachineBasicBlock::iterator
KlaussCPUFrameLowering::eliminateCallFramePseudoInstr(
    MachineFunction &MF, MachineBasicBlock &MBB,
    MachineBasicBlock::iterator I) const {
  // When hasReservedCallFrame() is true the call frame is already baked into
  // the prologue allocation, so these pseudos are just deleted.
  // When it returns false (VLA present) we emit ADDSP adjustments.
  if (!hasReservedCallFrame(MF)) {
    const KlaussCPUInstrInfo &TII =
        *MF.getSubtarget<KlaussCPUSubtarget>().getInstrInfo();
    MachineInstr &MI = *I;
    int64_t Amount = MI.getOperand(0).getImm();
    if (Amount != 0) {
      DebugLoc DL = MI.getDebugLoc();
      if (MI.getOpcode() == TII.getCallFrameSetupOpcode())
        BuildMI(MBB, I, DL, TII.get(KlaussCPU::ADDSP_I)).addImm(-Amount);
      else
        BuildMI(MBB, I, DL, TII.get(KlaussCPU::ADDSP_I)).addImm(Amount);
    }
  }
  return MBB.erase(I);
}

void KlaussCPUFrameLowering::emitPrologue(MachineFunction &MF,
                                           MachineBasicBlock &MBB) const {
  const KlaussCPUInstrInfo &TII =
      *MF.getSubtarget<KlaussCPUSubtarget>().getInstrInfo();
  MachineBasicBlock::iterator MBBI = MBB.begin();
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  DebugLoc DL;

  bool NeedsCFI = MF.needsFrameMoves();
  CFIInstBuilder CFIBuilder(MBB, MBBI, MachineInstr::FrameSetup);

  uint64_t FrameSize = MFI.getStackSize();
  if (EnableEnterLeave && fitsEnter(FrameSize)) {
    // push R15; R15 = SP; SP -= FrameSize — one word.
    BuildMI(MBB, MBBI, DL, TII.get(KlaussCPU::ENTER_I))
        .addImm(FrameSize / 8)
        .setMIFlag(MachineInstr::FrameSetup);
    if (NeedsCFI) {
      CFIBuilder.buildDefCFA(KlaussCPU::R15, 16);
      CFIBuilder.buildOffset(KlaussCPU::R15, -16);
    }
  } else {
    // Save the caller's frame pointer.
    BuildMI(MBB, MBBI, DL, TII.get(KlaussCPU::PUSH_R))
        .addReg(KlaussCPU::R15, RegState::Kill)
        .setMIFlag(MachineInstr::FrameSetup);
    if (NeedsCFI) {
      CFIBuilder.buildDefCFAOffset(16);
      CFIBuilder.buildOffset(KlaussCPU::R15, -16);
    }

    // Establish new frame pointer: R15 = SP.
    BuildMI(MBB, MBBI, DL, TII.get(KlaussCPU::GETSP_R), KlaussCPU::R15)
        .setMIFlag(MachineInstr::FrameSetup);
    if (NeedsCFI)
      CFIBuilder.buildDefCFARegister(KlaussCPU::R15);

    // Allocate stack frame for locals.
    if (FrameSize > 0)
      BuildMI(MBB, MBBI, DL, TII.get(KlaussCPU::ADDSP_I))
          .addImm(-(int64_t)FrameSize)
          .setMIFlag(MachineInstr::FrameSetup);
  }

  if (!NeedsCFI)
    return;

  // Callee-saved spills (spillCalleeSavedRegisters) follow the prologue; place
  // their .cfi_offset rules after the stores.  Spill slots are R15-relative
  // (eliminateFrameIndex), and R15 = CFA-16.
  const std::vector<CalleeSavedInfo> &CSI = MFI.getCalleeSavedInfo();
  while (MBBI != MBB.end() && MBBI->getFlag(MachineInstr::FrameSetup))
    ++MBBI;
  CFIBuilder.setInsertPoint(MBBI);
  for (const CalleeSavedInfo &CS : CSI)
    CFIBuilder.buildOffset(CS.getReg(),
                           MFI.getObjectOffset(CS.getFrameIdx()) - 16);
}

// Once the frame is gone, R15 and the callee-saved registers hold the
// caller's values again, and their spill slots are below SP (an interrupt
// may overwrite them), so drop the save rules.
static void buildFrameTornDownCFI(const MachineFunction &MF,
                                  const CFIInstBuilder &CFIBuilder) {
  CFIBuilder.buildSameValue(KlaussCPU::R15);
  for (const CalleeSavedInfo &CS : MF.getFrameInfo().getCalleeSavedInfo())
    CFIBuilder.buildSameValue(CS.getReg());
}

void KlaussCPUFrameLowering::emitEpilogue(MachineFunction &MF,
                                           MachineBasicBlock &MBB) const {
  const KlaussCPUInstrInfo &TII =
      *MF.getSubtarget<KlaussCPUSubtarget>().getInstrInfo();

  // Insert epilogue before the return instruction.
  MachineBasicBlock::iterator MBBI = MBB.getLastNonDebugInstr();
  DebugLoc DL = MBBI->getDebugLoc();

  // LEAVE = SETSP R15; POP R15 (valid whatever the prologue was: both leave
  // R15 pointing at the saved caller R15).
  bool NeedsCFI = MF.needsFrameMoves();
  CFIInstBuilder CFIBuilder(MBB, MBBI, MachineInstr::FrameDestroy);

  if (EnableEnterLeave) {
    // A plain return folds into LEAVERET (IRET / other terminators keep
    // LEAVE in front of them).
    if (EnableLeaveRet && MBBI->getOpcode() == KlaussCPU::RET_I) {
      // Keep RET's implicit uses (the return-value registers) so post-RA
      // passes still see them live out of this block.
      BuildMI(MBB, MBBI, DL, TII.get(KlaussCPU::LEAVERET_I))
          .copyImplicitOps(*MBBI)
          .setMIFlag(MachineInstr::FrameDestroy);
      MBB.erase(MBBI);
      return;
    }
    BuildMI(MBB, MBBI, DL, TII.get(KlaussCPU::LEAVE_I))
        .setMIFlag(MachineInstr::FrameDestroy);
    if (NeedsCFI) {
      CFIBuilder.buildDefCFA(KlaussCPU::SP, 8);
      buildFrameTornDownCFI(MF, CFIBuilder);
    }
    return;
  }

  // Restore SP from frame pointer (discards locals).
  BuildMI(MBB, MBBI, DL, TII.get(KlaussCPU::SETSP_R))
      .addReg(KlaussCPU::R15)
      .setMIFlag(MachineInstr::FrameDestroy);
  if (NeedsCFI)
    CFIBuilder.buildDefCFARegister(KlaussCPU::SP);

  // Restore caller's frame pointer.
  BuildMI(MBB, MBBI, DL, TII.get(KlaussCPU::POP_R), KlaussCPU::R15)
      .setMIFlag(MachineInstr::FrameDestroy);
  if (NeedsCFI) {
    CFIBuilder.buildDefCFAOffset(8);
    buildFrameTornDownCFI(MF, CFIBuilder);
  }
}

bool KlaussCPUFrameLowering::spillCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
    ArrayRef<CalleeSavedInfo> CSI, const TargetRegisterInfo *TRI) const {
  // Same stores as the generic code, but flagged FrameSetup so emitPrologue
  // can place the CSR .cfi_offset rules after them.
  const TargetInstrInfo &TII = *MBB.getParent()->getSubtarget().getInstrInfo();
  for (const CalleeSavedInfo &CS : CSI) {
    MCRegister Reg = CS.getReg();
    TII.storeRegToStackSlot(MBB, MI, Reg, /*isKill=*/true, CS.getFrameIdx(),
                            TRI->getMinimalPhysRegClass(Reg), Register(),
                            MachineInstr::FrameSetup);
  }
  return true;
}

bool KlaussCPUFrameLowering::restoreCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
    MutableArrayRef<CalleeSavedInfo> CSI,
    const TargetRegisterInfo *TRI) const {
  // The reloads need no CFI: the spill slots still hold the caller's values,
  // so the .cfi_offset rules stay valid until the frame is torn down.
  const TargetInstrInfo &TII = *MBB.getParent()->getSubtarget().getInstrInfo();
  for (const CalleeSavedInfo &CS : reverse(CSI)) {
    MCRegister Reg = CS.getReg();
    TII.loadRegFromStackSlot(MBB, MI, Reg, CS.getFrameIdx(),
                             TRI->getMinimalPhysRegClass(Reg), Register(),
                             /*SubReg=*/0, MachineInstr::FrameDestroy);
  }
  return true;
}

void KlaussCPUFrameLowering::resetCFIToInitialState(
    MachineBasicBlock &MBB) const {
  // Back to the CIE state: CFA = SP+8, every register holds its own value.
  CFIInstBuilder CFIBuilder(MBB, MBB.begin(), MachineInstr::NoFlags);
  CFIBuilder.buildDefCFA(KlaussCPU::SP, 8);
  buildFrameTornDownCFI(*MBB.getParent(), CFIBuilder);
}
