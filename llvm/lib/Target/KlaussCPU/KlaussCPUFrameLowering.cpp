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
//===----------------------------------------------------------------------===//

#include "KlaussCPUFrameLowering.h"
#include "KlaussCPUInstrInfo.h"
#include "KlaussCPUSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

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

  // Save the caller's frame pointer.
  BuildMI(MBB, MBBI, DL, TII.get(KlaussCPU::PUSH_R))
      .addReg(KlaussCPU::R15, RegState::Kill);

  // Establish new frame pointer: R15 = SP.
  BuildMI(MBB, MBBI, DL, TII.get(KlaussCPU::GETSP_R), KlaussCPU::R15);

  // Allocate stack frame for locals.
  uint64_t FrameSize = MFI.getStackSize();
  if (FrameSize > 0)
    BuildMI(MBB, MBBI, DL, TII.get(KlaussCPU::ADDSP_I))
        .addImm(-(int64_t)FrameSize);
}

void KlaussCPUFrameLowering::emitEpilogue(MachineFunction &MF,
                                           MachineBasicBlock &MBB) const {
  const KlaussCPUInstrInfo &TII =
      *MF.getSubtarget<KlaussCPUSubtarget>().getInstrInfo();

  // Insert epilogue before the return instruction.
  MachineBasicBlock::iterator MBBI = MBB.getLastNonDebugInstr();
  DebugLoc DL = MBBI->getDebugLoc();

  // Restore SP from frame pointer (discards locals).
  BuildMI(MBB, MBBI, DL, TII.get(KlaussCPU::SETSP_R))
      .addReg(KlaussCPU::R15);

  // Restore caller's frame pointer.
  BuildMI(MBB, MBBI, DL, TII.get(KlaussCPU::POP_R), KlaussCPU::R15);
}
