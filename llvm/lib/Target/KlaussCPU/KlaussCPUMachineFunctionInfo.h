//===-- KlaussCPUMachineFunctionInfo.h - KlaussCPU per-function info -*- C++ -*-===//
//
// KlaussCPU LLVM backend — per-function machine state.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUMACHINEFUNCTIONINFO_H
#define LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUMACHINEFUNCTIONINFO_H

#include "llvm/CodeGen/MachineFunction.h"

namespace llvm {

class KlaussCPUMachineFunctionInfo : public MachineFunctionInfo {
  // Frame index of the first variadic argument (the va_list address points here).
  // Set only for varargs functions; 0 otherwise.
  int VarArgsFrameIndex = 0;

public:
  KlaussCPUMachineFunctionInfo(const Function &, const TargetSubtargetInfo *) {}

  MachineFunctionInfo *
  clone(BumpPtrAllocator &Allocator, MachineFunction &DestMF,
        const DenseMap<MachineBasicBlock *, MachineBasicBlock *> &) const override {
    return DestMF.cloneInfo<KlaussCPUMachineFunctionInfo>(*this);
  }

  int  getVarArgsFrameIndex() const        { return VarArgsFrameIndex; }
  void setVarArgsFrameIndex(int Idx)       { VarArgsFrameIndex = Idx; }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUMACHINEFUNCTIONINFO_H
