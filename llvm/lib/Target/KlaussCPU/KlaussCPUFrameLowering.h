//===-- KlaussCPUFrameLowering.h - KlaussCPU Frame Lowering -----*- C++ -*-===//
//
// KlaussCPU LLVM backend — frame lowering class declaration.
//
// KlaussCPU always uses a frame pointer (R15 = P).  Frame slots are addressed
// P+offset via LDIDX64/STIDX64, except in a function that realigns its stack
// (a local aligned above 8 bytes), whose locals are addressed from a base
// pointer (R7 = H) holding P rounded down to that alignment.  The hardware
// stack pointer (SP) is not a GPR: only GETSP / SETSP / ADDSP reach it.
//
// Stack grows downward; the ABI stack alignment is 8 bytes.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUFRAMELOWERING_H
#define LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUFRAMELOWERING_H

#include "llvm/CodeGen/TargetFrameLowering.h"
#include "llvm/TargetParser/KlaussCPUABI.h"

namespace llvm {

class KlaussCPUSubtarget;

class KlaussCPUFrameLowering : public TargetFrameLowering {
public:
  explicit KlaussCPUFrameLowering(const KlaussCPUSubtarget &STI)
      : TargetFrameLowering(StackGrowsDown,
                            /*StackAlignment=*/Align(KlaussCPUABI::StackAlign),
                            /*LocalAreaOffset=*/0) {}

  void emitPrologue(MachineFunction &MF,
                    MachineBasicBlock &MBB) const override;

  void emitEpilogue(MachineFunction &MF,
                    MachineBasicBlock &MBB) const override;

  // Reserve a call frame at the bottom of each function's stack frame so that
  // CALLSEQ_START/END pseudos need not emit SP adjustments.  This requires the
  // prologue to include MFI.getMaxCallFrameSize() in its ADDSP allocation.
  bool hasReservedCallFrame(const MachineFunction &MF) const override;

  // Expand or delete CALLSEQ_START/END pseudo-instructions.
  MachineBasicBlock::iterator eliminateCallFramePseudoInstr(
      MachineFunction &MF, MachineBasicBlock &MBB,
      MachineBasicBlock::iterator I) const override;

  // Callee-saved spills/reloads flagged FrameSetup/FrameDestroy so the CFI
  // for them can be placed (see emitPrologue).
  bool spillCalleeSavedRegisters(MachineBasicBlock &MBB,
                                 MachineBasicBlock::iterator MI,
                                 ArrayRef<CalleeSavedInfo> CSI,
                                 const TargetRegisterInfo *TRI) const override;
  bool
  restoreCalleeSavedRegisters(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator MI,
                              MutableArrayRef<CalleeSavedInfo> CSI,
                              const TargetRegisterInfo *TRI) const override;

  // Frame objects are addressed R15-relative, or BP-relative for the locals of
  // a realigning function.  This is the one place the base and offset are
  // computed: eliminateFrameIndex, the CSR CFI and the debug-info locations
  // (DW_OP_fbreg against DW_AT_frame_base = R15, or DW_OP_breg7) all use it.
  StackOffset getFrameIndexReference(const MachineFunction &MF, int FI,
                                     Register &FrameReg) const override;

  // A realigning function also saves and restores the base pointer.
  void determineCalleeSaves(MachineFunction &MF, BitVector &SavedRegs,
                            RegScavenger *RS) const override;

  // True if the function realigns its stack, and so reserves the base pointer
  // (getBPReg) to address its locals.
  bool hasBP(const MachineFunction &MF) const;
  static Register getBPReg();

  // CFIFixup hook: reset the unwind state to the CIE's (CFA = SP+8).
  void resetCFIToInitialState(MachineBasicBlock &MBB) const override;

  // KlaussCPU always frames (R15 = P is always the frame pointer).
  bool hasFPImpl(const MachineFunction &MF) const override { return true; }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUFRAMELOWERING_H
