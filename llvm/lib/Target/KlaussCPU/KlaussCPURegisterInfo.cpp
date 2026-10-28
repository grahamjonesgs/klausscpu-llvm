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
//   R15 — frame pointer; a frameless leaf leaves the caller's value in it
//   R7  — base pointer, only in a function that realigns its stack
//
//===----------------------------------------------------------------------===//

#include "KlaussCPURegisterInfo.h"
#include "KlaussCPUFrameLowering.h"
#include "KlaussCPUSubtarget.h"
#include "llvm/CodeGen/LivePhysRegs.h"
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

// A call preserves R4–R7, R15 and SP (CSR_KlaussCPU_Call) and clobbers every
// other register (and FLAGS).
const uint32_t *
KlaussCPURegisterInfo::getCallPreservedMask(const MachineFunction &MF,
                                             CallingConv::ID) const {
  return CSR_KlaussCPU_Call_RegMask;
}

BitVector
KlaussCPURegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  BitVector Reserved(getNumRegs());
  Reserved.set(KlaussCPU::SP);    // hardware stack pointer
  Reserved.set(KlaussCPU::R15);   // frame pointer (also in frameless leaves)
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

// Is operand FIOp of MI a base whose next operand is an offset added to it:
// the ADDI frame address (AddrFI), a base+offset load or store, or an inline
// asm memory operand (SelectInlineAsmMemoryOperand)?  Anything else with a
// frame index -- MEMGETn/MEMSETn, the reg+reg LDIDXnR/STIDXnR, an operand
// whose next immediate is not an offset -- gets the frame address in a
// register instead.
static bool isBaseOffsetOperand(const MachineInstr &MI, unsigned FIOp) {
  if (MI.isInlineAsm())
    return FIOp + 1 < MI.getNumOperands() && MI.getOperand(FIOp + 1).isImm();
  switch (MI.getOpcode()) {
  case KlaussCPU::ADDI:
  case KlaussCPU::LDIDX64:
  case KlaussCPU::LDIDX64U:
  case KlaussCPU::LDIDX32:
  case KlaussCPU::LDIDX32_S:
  case KlaussCPU::LDIDX16:
  case KlaussCPU::LDIDX16_S:
  case KlaussCPU::LDIDX8:
  case KlaussCPU::LDIDX8_S:
  case KlaussCPU::STIDX64:
  case KlaussCPU::STIDX64U:
  case KlaussCPU::STIDX32:
  case KlaussCPU::STIDX16:
  case KlaussCPU::STIDX8:
    // (rd, base, offset) / (data, base, offset).
    return FIOp == 1;
  default:
    return false;
  }
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

  // Standard case: (base, imm_offset) at [FIOperandNum, FIOperandNum+1].
  // Replace FI with FrameReg (R15, or the base pointer R7) and add the
  // frame-slot offset to the offset isel folded in (AddrFI).
  if (isBaseOffsetOperand(MI, FIOperandNum)) {
    int64_t Offset = BaseOffset + MI.getOperand(FIOperandNum + 1).getImm();
    if (isInt<32>(Offset)) {
      MI.getOperand(FIOperandNum).ChangeToRegister(FrameReg, false);
      MI.getOperand(FIOperandNum + 1).ChangeToImmediate(Offset);
      // A frame address 129..255 bytes below the frame register: MINUSV of
      // the negated offset compresses to 1 word, ADDI only down to -128.
      // The result is the same; C would be the borrow instead of the carry,
      // so only while the FLAGS def is dead (isel never reads it).
      if (MI.getOpcode() == KlaussCPU::ADDI && Offset >= -255 &&
          Offset <= -129 && MI.registerDefIsDead(KlaussCPU::FLAGS, this)) {
        MI.setDesc(TII.get(KlaussCPU::MINUSV));
        MI.getOperand(FIOperandNum + 1).setImm(-Offset);
      }
      return false;
    }
    // The sum overflows simm32.  Isel folds at most a 512 MiB offset onto a
    // frame index (isFrameIndexOffset in KlaussCPUISelDAGToDAG.cpp), so this
    // needs a frame over 1.5 GiB.  Keep the instruction's offset and put the
    // slot address in a scratch register, as below.
  }

  // Non-standard case: a frame index as a plain register operand.
  // Materialise (FrameReg + BaseOffset) into a scratch register with a
  // single ADDI, then replace the FI operand.
  assert(RS && "Register scavenger required for variable-indexed stack access");
  assert(isInt<32>(BaseOffset) && "Frame offset out of ADDI simm32 range");

  // The ADDI writes FLAGS, and the ISA has no flag-free add.  Isel puts a
  // frame index only where the code above handles it, so this is reached
  // only for a frame over 1.5 GiB, and the instruction is a load or store
  // that the scheduler may have put between a compare and its reader.
  // Refuse rather than clobber those flags.
  LivePhysRegs LiveRegs(*this);
  LiveRegs.addLiveOuts(MBB);
  for (MachineInstr &I : make_range(MBB.rbegin(), std::next(II.getReverse())))
    LiveRegs.stepBackward(I);
  if (LiveRegs.contains(KlaussCPU::FLAGS))
    reportFatalUsageError("KlaussCPU: frame offset out of simm32 range where "
                          "FLAGS is live");

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

// The frame base of the debug info (DW_AT_frame_base): R15, or SP in a
// frameless leaf, which has no frame-relative locations.
Register
KlaussCPURegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  return getFrameLowering(MF)->hasFP(MF) ? KlaussCPU::R15 : KlaussCPU::SP;
}
