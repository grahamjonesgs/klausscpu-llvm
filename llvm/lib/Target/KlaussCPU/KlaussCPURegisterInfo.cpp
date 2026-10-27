//===-- KlaussCPURegisterInfo.cpp - KlaussCPU Register Information --------===//
//
// KlaussCPU LLVM backend — register info implementation.
//
// Calling convention (KlaussCPUCallingConv.td):
//   arg 0–3    : R0–R3   (A–D)   caller-saved
//   return     : R12, R11 (M, L) caller-saved
//   callee-saved: R4–R7  (E–H) + R15 (P = frame pointer)
//   temporaries : R8–R11, R13–R14 (I–L, N–O)  caller-saved
//
// Reserved registers:
//   SP  — hardware stack pointer, never visible to register allocator
//   R15 — frame pointer, always active (we always emit a frame pointer)
//   R7  — base pointer, only in a function that realigns its stack
//
//===----------------------------------------------------------------------===//

#include "KlaussCPURegisterInfo.h"
#include "KlaussCPUFrameLowering.h"
#include "KlaussCPUSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"

#define GET_REGINFO_TARGET_DESC
#include "KlaussCPUGenRegisterInfo.inc"

using namespace llvm;

KlaussCPURegisterInfo::KlaussCPURegisterInfo()
    : KlaussCPUGenRegisterInfo(/*RA=*/0) {}

// CSR_KlaussCPU (KlaussCPUCallingConv.td): R4–R7.  R15, the frame pointer,
// is reserved and saved by the prologue itself (ENTER / PUSH R15), so it is
// not in the list.
const MCPhysReg *
KlaussCPURegisterInfo::getCalleeSavedRegs(const MachineFunction *) const {
  return CSR_KlaussCPU_SaveList;
}

const uint32_t *
KlaussCPURegisterInfo::getCallPreservedMask(const MachineFunction &MF,
                                             CallingConv::ID) const {
  return CSR_KlaussCPU_RegMask;
}

BitVector
KlaussCPURegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  BitVector Reserved(getNumRegs());
  Reserved.set(KlaussCPU::SP);    // hardware stack pointer
  Reserved.set(KlaussCPU::R15);   // frame pointer (always active)
  // Base pointer, only in a function that realigns its stack.
  const KlaussCPUFrameLowering *TFI =
      MF.getSubtarget<KlaussCPUSubtarget>().getFrameLowering();
  if (TFI->hasBP(MF))
    Reserved.set(TFI->getBPReg());
  // FLAGS is deliberately NOT reserved (it is in no allocatable class, so it is
  // still never allocated).  Reserving it excludes it from the machine
  // scheduler's block-boundary liveness, so a conditional branch's FLAGS use (a
  // terminator, outside the scheduling region) was not seen as keeping FLAGS
  // live-out — and a flag-clobbering arithmetic op (e.g. INCR from `*p++`) could
  // be scheduled into the compare->branch gap, making the branch read the wrong
  // flags (board itoa/printf infinite loop).  Tracking FLAGS as a normal physreg
  // (the X86 EFLAGS model) makes that live range visible so nothing clobbers it.
  return Reserved;
}

bool KlaussCPURegisterInfo::shouldRealignStack(
    const MachineFunction &MF) const {
  const Function &F = MF.getFunction();
  if (F.hasFnAttribute("stackrealign") ||
      F.hasFnAttribute(Attribute::StackAlignment))
    return true;
  // MachineFrameInfo's MaxAlign also counts dynamic allocas, but
  // LowerDYNAMIC_STACKALLOC aligns SP for those, so only an over-aligned
  // static object needs the base pointer.
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  Align StackAlign = MF.getSubtarget().getFrameLowering()->getStackAlign();
  for (int FI = 0, E = MFI.getObjectIndexEnd(); FI != E; ++FI)
    if (!MFI.isVariableSizedObjectIndex(FI) &&
        MFI.getObjectAlign(FI) > StackAlign)
      return true;
  return false;
}

bool KlaussCPURegisterInfo::canRealignStack(const MachineFunction &MF) const {
  // Realignment needs the base pointer, which can no longer be reserved once
  // register allocation has started without it.
  return TargetRegisterInfo::canRealignStack(MF) &&
         MF.getRegInfo().canReserveReg(KlaussCPUFrameLowering::getBPReg());
}

bool KlaussCPURegisterInfo::isAsmClobberable(const MachineFunction &MF,
                                             MCRegister PhysReg) const {
  // Warn about clobbering SP, R15 or the base pointer: inline asm that changes
  // them breaks the frame.
  return !getReservedRegs(MF).test(PhysReg.id());
}

bool KlaussCPURegisterInfo::isInlineAsmReadOnlyReg(const MachineFunction &MF,
                                                   MCRegister PhysReg) const {
  // An asm operand that writes the frame or base pointer is an error.
  return PhysReg == KlaussCPU::R15 ||
         (PhysReg == KlaussCPUFrameLowering::getBPReg().asMCReg() &&
          MF.getSubtarget<KlaussCPUSubtarget>().getFrameLowering()->hasBP(MF));
}

bool KlaussCPURegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator II,
                                                 int SPAdj,
                                                 unsigned FIOperandNum,
                                                 RegScavenger *RS) const {
  assert(SPAdj == 0 && "Unexpected SPAdj");
  MachineInstr &MI = *II;
  MachineBasicBlock &MBB = *MI.getParent();
  MachineFunction &MF = *MBB.getParent();
  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();

  // The frame-slot offset comes from getFrameIndexReference, which the debug
  // info (DW_OP_fbreg) uses too, so the two cannot disagree.
  int FrameIndex = MI.getOperand(FIOperandNum).getIndex();
  Register FrameReg;
  int64_t BaseOffset =
      MF.getSubtarget()
          .getFrameLowering()
          ->getFrameIndexReference(MF, FrameIndex, FrameReg)
          .getFixed();

  // Standard case: load/store instructions have (base, imm_offset) at
  // [FIOperandNum, FIOperandNum+1].  Replace FI with FrameReg (R15, or the
  // base pointer R7) and fold in the frame-slot offset.
  if (FIOperandNum + 1 < MI.getNumOperands() &&
      MI.getOperand(FIOperandNum + 1).isImm()) {
    int64_t Offset = BaseOffset + MI.getOperand(FIOperandNum + 1).getImm();
    if (isInt<32>(Offset)) {
      MI.getOperand(FIOperandNum).ChangeToRegister(FrameReg, false);
      MI.getOperand(FIOperandNum + 1).ChangeToImmediate(Offset);
      return false;
    }
    // The sum overflows simm32.  Isel folds at most a 512 MiB offset onto a
    // frame index (SelectInlineAsmMemoryOperand), so this needs a frame over
    // 1.5 GiB.  Keep the instruction's offset and put the slot address in a
    // scratch register, as below.
  }

  // Non-standard case: FrameIndex used as a plain register operand (e.g. ADDR
  // for variable-indexed stack access).  Materialise (FrameReg + BaseOffset)
  // into a scratch register with a single ADDI, then replace the FI operand.
  assert(RS && "Register scavenger required for variable-indexed stack access");
  assert(isInt<32>(BaseOffset) && "Frame offset out of ADDI simm32 range");

  DebugLoc DL = MI.getDebugLoc();
  Register Scratch = RS->scavengeRegisterBackwards(
      KlaussCPU::GPRRegClass, II, /*RestoreAfter=*/false, SPAdj);

  BuildMI(MBB, II, DL, TII.get(KlaussCPU::ADDI), Scratch)
      .addReg(FrameReg)
      .addImm(BaseOffset);

  MI.getOperand(FIOperandNum).ChangeToRegister(Scratch, /*isDef=*/false,
                                               /*isImp=*/false,
                                               /*isKill=*/true);
  return false;
}

Register
KlaussCPURegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  return KlaussCPU::R15; // P
}
