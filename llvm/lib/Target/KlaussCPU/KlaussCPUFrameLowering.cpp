//===-- KlaussCPUFrameLowering.cpp - KlaussCPU Frame Lowering -------------===//
//
// KlaussCPU LLVM backend — frame lowering implementation.
//
// Frame layout (stack grows down).  CALL pushes the return address, so the
// callee is entered with SP (the incoming SP) pointing at it:
//
//   High addresses (caller)
//   ┌──────────────┐
//   │  Stack args  │    [incoming SP+32 ..]  (8-byte slots)
//   ├──────────────┤
//   │   (reserved) │    [incoming SP+8 .. +31]  R1-R3 home area (varargs)
//   ├──────────────┤
//   │  Return addr │    [incoming SP+0 .. +7]   (written by CALL)
//   ├──────────────┤  ← incoming SP
//   │   Saved R15  │    [R15+0 .. +7]
//   ├──────────────┤  ← R15 = incoming SP - 8  (frame pointer)
//   │ Callee-saved │
//   │ Locals, call │    [R15 - stack size .. R15 - 8]
//   │ frames       │
//   └──────────────┘  ← SP after the prologue
//
// Prologue:  ENTER N       ; push R15; R15 = SP; SP -= 8*N   (N = size / 8)
//            <callee-saved spills, R15-relative>
// Epilogue:  <callee-saved reloads>
//            LEAVERET      ; SP = R15; pop R15; RET   (LEAVE in front of any
//                          ; other terminator, e.g. IRET)
// A frame too large for ENTER's 22-bit field is set up with PUSH R15;
// GETSP R15; ADDSP -size (LEAVE still tears it down).  With
// -klausscpu-enter-leave=false every frame uses that prologue and the
// epilogue SETSP R15; POP R15.
//
// Stack realignment.  The ABI keeps SP 8-byte aligned, so R15 is only 8-byte
// aligned.  A function with a local aligned to A > 8 (_Alignas(32), an
// __int128, an over-aligned byval copy) reserves R7 as a base pointer (BP):
//
//            ENTER (N + A - 8) / 8           ; frame plus realignment slack
//            <callee-saved spills>           ; R15-relative, saves the old BP
//            ANDI  R7, R15, -A               ; BP = R15 rounded down to A
//
// PEI lays every local out at an offset that is a multiple of its alignment
// below a MaxAlign-aligned top, so BP+offset is aligned, and BP is at most
// A-8 below R15, which the slack covers.  The locals stay below the
// callee-saved slots, so the two areas never overlap.  Incoming arguments and
// the callee-saved slots stay R15-relative, SP is not realigned (the outgoing
// call area only needs 8 bytes; dynamic allocas align themselves, so an
// over-aligned one alone does not realign the frame), and BP is unaffected by
// VLAs.  The epilogue is unchanged: LEAVE restores SP from R15, and the CFA
// stays R15-based.  Inline asm cannot keep BP live, so writing R7 through an
// asm operand is an error and an R7 clobber draws the reserved-register
// warning (KlaussCPURegisterInfo).
//
// Call frame information (emitted when MF.needsFrameMoves(), i.e. with -g or
// unwind tables).  CFA = the caller's SP before its CALL.  The CIE (see
// createKlaussCPUMCAsmInfo) starts each function at CFA = SP+8 with the return
// address (RA column, DWARF 17) at CFA-8.  After the prologue CFA = R15+16
// with the caller's R15 at CFA-16; each callee-saved spill is followed by its
// .cfi_offset rule, placed after post-RA scheduling by KlaussCPUCalleeSavedCFI
// so that -g does not change the schedule (CFI instructions are scheduling
// boundaries; the rest of the frame's CFI follows instructions that write SP,
// which are boundaries anyway).
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
#include "llvm/TargetParser/KlaussCPUABI.h"

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

// The prologue saves the caller's R15 just below the return address.
static constexpr int SavedFPCFAOffset =
    -static_cast<int>(KlaussCPUABI::FPToCFAOffset);

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
    Offset += KlaussCPUABI::SavedFPSize;
  // In a realigning function the locals are BP-relative.  The callee-saved
  // slots stay R15-relative: they are written before BP is set up.
  else if (hasBP(MF) && !MFI.isCalleeSavedObjectIndex(FI))
    FrameReg = getBPReg();
  return StackOffset::getFixed(Offset);
}

Register KlaussCPUFrameLowering::getBPReg() { return KlaussCPU::R7; }

bool KlaussCPUFrameLowering::hasBP(const MachineFunction &MF) const {
  // Every frame access goes through a register, and SP is not one, so any
  // realignment needs the base pointer.  Only static objects ask for it
  // (KlaussCPURegisterInfo::shouldRealignStack).
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
    MFI.setStackSize(MFI.getStackSize() + Alignment.value() -
                     getStackAlign().value());

  uint64_t FrameSize = MFI.getStackSize();
  if (EnableEnterLeave && fitsEnter(FrameSize)) {
    // push R15; R15 = SP; SP -= FrameSize — one word.
    BuildMI(MBB, MBBI, DL, TII.get(KlaussCPU::ENTER_I))
        .addImm(FrameSize / 8)
        .setMIFlag(MachineInstr::FrameSetup);
    if (NeedsCFI) {
      CFIBuilder.buildDefCFA(KlaussCPU::R15, KlaussCPUABI::FPToCFAOffset);
      CFIBuilder.buildOffset(KlaussCPU::R15, SavedFPCFAOffset);
    }
  } else {
    // Save the caller's frame pointer.
    BuildMI(MBB, MBBI, DL, TII.get(KlaussCPU::PUSH_R))
        .addReg(KlaussCPU::R15, RegState::Kill)
        .setMIFlag(MachineInstr::FrameSetup);
    if (NeedsCFI) {
      CFIBuilder.buildDefCFAOffset(KlaussCPUABI::FPToCFAOffset);
      CFIBuilder.buildOffset(KlaussCPU::R15, SavedFPCFAOffset);
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

  if (!HasBP)
    return;

  // Callee-saved spills (spillCalleeSavedRegisters) follow the prologue.
  while (MBBI != MBB.end() && MBBI->getFlag(MachineInstr::FrameSetup))
    ++MBBI;

  // Set up BP after the spills, which save the caller's value of it.
  if (HasBP) {
    // IR allows alignments up to 2^32, whose mask would be 0.
    if (!isUInt<32>(Alignment.value()))
      reportFatalUsageError("KlaussCPU: stack object alignment exceeds the "
                            "32-bit address space");
    // ANDI sign-extends its immediate (1 word for A <= 128) and leaves the
    // flags alone.
    BuildMI(MBB, MBBI, DL, TII.get(KlaussCPU::ANDI), getBPReg())
        .addReg(KlaussCPU::R15)
        .addImm(-static_cast<int64_t>(Alignment.value()))
        .setMIFlag(MachineInstr::FrameSetup);
  }

  // The callee-saved registers' .cfi_offset rules are placed behind their
  // spills after the post-RA scheduler (KlaussCPUCalleeSavedCFI.cpp).
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
      CFIBuilder.buildDefCFA(KlaussCPU::SP, KlaussCPUABI::RetAddrSize);
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
    CFIBuilder.buildDefCFAOffset(KlaussCPUABI::RetAddrSize);
    buildFrameTornDownCFI(MF, CFIBuilder);
  }
}

bool KlaussCPUFrameLowering::spillCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
    ArrayRef<CalleeSavedInfo> CSI, const TargetRegisterInfo *TRI) const {
  // Same stores as the generic code, but flagged FrameSetup so emitPrologue
  // can place the BP setup and KlaussCPUCalleeSavedCFI the CSR .cfi_offset
  // rules after them.
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
  CFIBuilder.buildDefCFA(KlaussCPU::SP, KlaussCPUABI::RetAddrSize);
  buildFrameTornDownCFI(*MBB.getParent(), CFIBuilder);
}
