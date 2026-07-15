//===-- KlaussCPUSubtarget.h - KlaussCPU Subtarget Info ---------*- C++ -*-===//
//
// KlaussCPU LLVM backend — subtarget class declaration.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUSUBTARGET_H
#define LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUSUBTARGET_H

#include "KlaussCPUFrameLowering.h"
#include "KlaussCPUInstrInfo.h"
#include "KlaussCPUISelLowering.h"
#include "KlaussCPURegisterInfo.h"
#include "llvm/CodeGen/SelectionDAGTargetInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Target/TargetMachine.h"

#define GET_SUBTARGETINFO_HEADER
#include "KlaussCPUGenSubtargetInfo.inc"

namespace llvm {

class KlaussCPUSubtarget : public KlaussCPUGenSubtargetInfo {
  // Member declaration order matters: RI must precede II so it is
  // initialized first (used as a constructor argument to II).
  KlaussCPURegisterInfo     RI;
  KlaussCPUInstrInfo        II;
  KlaussCPUFrameLowering    FL;
  KlaussCPUTargetLowering   TLI;
  SelectionDAGTargetInfo    TSI;
  const TargetMachine      &TM;

public:
  KlaussCPUSubtarget(const Triple &TT, StringRef CPU, StringRef FS,
                     const TargetMachine &TM);

  void ParseSubtargetFeatures(StringRef CPU, StringRef TuneCPU, StringRef FS);

  const KlaussCPURegisterInfo  *getRegisterInfo()  const override { return &RI;  }
  const KlaussCPUInstrInfo     *getInstrInfo()      const override { return &II;  }
  const TargetFrameLowering    *getFrameLowering()  const override { return &FL;  }
  const KlaussCPUTargetLowering*getTargetLowering() const override { return &TLI; }
  const SelectionDAGTargetInfo *getSelectionDAGInfo()const override { return &TSI; }

  // True when -fPIC / -relocation-model=pic is active.
  bool isPositionIndependent() const;

  // Enable the pre-RA machine scheduler (MISched) so the M8 latency model actually
  // spreads dependent chains before register allocation — the primary M8 lever.
  // TargetSubtargetInfo defaults this to false, which silently skips MachineScheduler
  // (MachineScheduler.cpp) and leaves only the post-RA list scheduler consuming the
  // latencies after regalloc has already pinned much of the order.  Keyed on the same
  // switch as the post-RA scheduler and the TTI unroll re-tune, so `-mcpu=no-sched`
  // (NoSchedModel) reverts pre-RA MISched too.  Side effects are standard: ISel falls
  // back to source-order SDAG scheduling and enableJoinGlobalCopies() turns on with it.
  bool enableMachineScheduler() const override {
    return getSchedModel().hasInstrSchedModel();
  }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUSUBTARGET_H
