//===-- KlaussCPUFrameLowering.h - KlaussCPU Frame Lowering -----*- C++ -*-===//
//
// KlaussCPU LLVM backend — frame lowering class declaration.
//
// KlaussCPU always uses a frame pointer (R15 = P).  All frame-slot accesses
// use P+offset via LDIDX64/STIDX64.  The hardware stack pointer (SP) is only
// touched in the prologue (GETSP / ADDSP) and epilogue (SETSP / POP P).
//
// Stack grows downward; slots are 8-byte aligned.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUFRAMELOWERING_H
#define LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUFRAMELOWERING_H

#include "llvm/CodeGen/TargetFrameLowering.h"

namespace llvm {

class KlaussCPUSubtarget;

class KlaussCPUFrameLowering : public TargetFrameLowering {
public:
  explicit KlaussCPUFrameLowering(const KlaussCPUSubtarget &STI)
      : TargetFrameLowering(StackGrowsDown,
                            /*StackAlignment=*/Align(8),
                            /*LocalAreaOffset=*/0) {}

  void emitPrologue(MachineFunction &MF,
                    MachineBasicBlock &MBB) const override;

  void emitEpilogue(MachineFunction &MF,
                    MachineBasicBlock &MBB) const override;

  // KlaussCPU always frames (R15 = P is always the frame pointer).
  bool hasFPImpl(const MachineFunction &MF) const override { return true; }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUFRAMELOWERING_H
