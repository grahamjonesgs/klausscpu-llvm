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
// Stack realignment.  The ABI keeps SP 8-byte aligned, so R15 is only 8-byte
// aligned.  A function with a local aligned to A > 8 (_Alignas(32), an
// __int128, an over-aligned byval copy) reserves R7 as a base pointer (BP):
//
//            ENTER (N + A - 8) / 8           ; frame plus realignment slack
//            <callee-saved spills>           ; R15-relative, saves the old BP
//            ANDV  R7, R15, -A               ; BP = R15 rounded down to A
//
// PEI lays every local out at an offset that is a multiple of its alignment
// below a MaxAlign-aligned top, so BP+offset is aligned, and BP is at most
// A-8 below R15, which the slack covers.  The locals stay below the
// callee-saved slots, so the two areas never overlap.  Incoming arguments and
// the callee-saved slots stay R15-relative, SP is not realigned (the outgoing
// call area only needs 8 bytes; dynamic allocas align themselves), and BP is
// unaffected by VLAs.  The epilogue is unchanged: LEAVE restores SP from R15,
// and the CFA stays R15-based.
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

StackOffset
KlaussCPUFrameLowering::getFrameIndexReference(const MachineFunction &MF,
                                               int FI,
                                               Register &FrameReg) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  FrameReg = KlaussCPU::R15;
  int64_t Offset = MFI.getObjectOffset(FI);
  // Fixed objects (incoming stack args, vararg register save slots) have
  // offsets relative to the incoming SP; R15 = incoming SP - 8 (the prologue
  // pushes the caller's R15 first).  Locals are laid out below the saved R15,
  // so their offsets are R15-relative already.
  if (MFI.isFixedObjectIndex(FI))
    Offset += 8;
  // In a realigning function the locals are BP-relative.  The callee-saved
  // slots stay R15-relative: they are written before BP is set up.
  else if (hasBP(MF) && !MFI.isCalleeSavedObjectIndex(FI))
    FrameReg = getBPReg();
  return StackOffset::getFixed(Offset);
}

Register KlaussCPUFrameLowering::getBPReg() { return KlaussCPU::R7; }

bool KlaussCPUFrameLowering::hasBP(const MachineFunction &MF) const {
  // Every frame access goes through a register, and SP is not one, so any
  // realignment needs the base pointer.
  return MF.getSubtarget().getRegisterInfo()->hasStackRealignment(MF);
}

void KlaussCPUFrameLowering::determineCalleeSaves(MachineFunction &MF,
                                                  BitVector &SavedRegs,
                                                  RegScavenger *RS) const {
  TargetFrameLowering::determineCalleeSaves(MF, SavedRegs, RS);
  // BP is reserved, so the register allocator never marks it modified; the
  // prologue overwrites it.  A naked function has no prologue.
  if (hasBP(MF) && !MF.getFunction().hasFnAttribute(Attribute::Naked))
    SavedRegs.set(getBPReg());
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
  MachineFrameInfo &MFI = MF.getFrameInfo();
  DebugLoc DL;

  bool NeedsCFI = MF.needsFrameMoves();
  CFIInstBuilder CFIBuilder(MBB, MBBI, MachineInstr::FrameSetup);

  // A realigning function addresses its locals from BP = R15 & -Alignment,
  // up to Alignment-8 bytes below R15: allocate that much more.
  bool HasBP = hasBP(MF);
  Align Alignment = std::max(MFI.getMaxAlign(), getStackAlign());
  if (HasBP)
    MFI.setStackSize(MFI.getStackSize() + Alignment.value() - 8);

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

  if (!HasBP && !NeedsCFI)
    return;

  // Callee-saved spills (spillCalleeSavedRegisters) follow the prologue.
  while (MBBI != MBB.end() && MBBI->getFlag(MachineInstr::FrameSetup))
    ++MBBI;

  // Set up BP after the spills, which save the caller's value of it.
  if (HasBP) {
    assert(isUInt<32>(Alignment.value()) && "Stack alignment too large");
    const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
    // ANDV zero-extends its immediate; addresses are 32-bit.
    BuildMI(MBB, MBBI, DL, TII.get(KlaussCPU::ANDV), getBPReg())
        .addReg(KlaussCPU::R15)
        .addImm(Lo_32(-Alignment.value()))
        .setMIFlag(MachineInstr::FrameSetup)
        ->addRegisterDead(KlaussCPU::FLAGS, TRI);
  }

  if (!NeedsCFI)
    return;

  // Place the .cfi_offset rules for the callee-saved registers after the
  // spills.  Spill slots are R15-relative (getFrameIndexReference), and
  // R15 = CFA-16.
  CFIBuilder.setInsertPoint(MBBI);
  for (const CalleeSavedInfo &CS : MFI.getCalleeSavedInfo()) {
    Register FrameReg;
    StackOffset Offset =
        getFrameIndexReference(MF, CS.getFrameIdx(), FrameReg);
    assert(FrameReg == KlaussCPU::R15 && "Callee-saved slot not R15-relative");
    CFIBuilder.buildOffset(CS.getReg(), Offset.getFixed() - 16);
  }
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
