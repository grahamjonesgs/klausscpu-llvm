//===-- KlaussCPUTargetTransformInfo.h - KlaussCPU TTI ---------*- C++ -*-===//
//
// KlaussCPU LLVM backend — target-specific TargetTransformInfo.
//
// The generic (BasicTTI) cost model assumes a conventional load/store machine
// with a fast instruction stream, so mid-level passes happily grow IR (partial
// and runtime loop unrolling, loop peeling) to trade code size for fewer
// dynamic branches.  KlaussCPU is *fetch-bound*: 59-75% of cycles are spent in
// the fetch FSM and the instruction fetch buffer is a single 16-byte line, so
// every extra instruction word fetched costs cycles.  Growing the static
// instruction count is therefore usually a net loss, not a win.
//
// This TTI subclass keeps the generic cost model but overrides the loop
// transform preferences to stop the size-growing unroll/peel heuristics.  Full
// unrolling of tiny constant-trip loops is left enabled (it removes the
// back-branch without adding a remainder loop), only partial/runtime unrolling
// and peeling — which duplicate the loop body — are disabled.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUTARGETTRANSFORMINFO_H
#define LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUTARGETTRANSFORMINFO_H

#include "KlaussCPUTargetMachine.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/BasicTTIImpl.h"

namespace llvm {

class KlaussCPUTTIImpl final : public BasicTTIImplBase<KlaussCPUTTIImpl> {
  using BaseT = BasicTTIImplBase<KlaussCPUTTIImpl>;
  using TTI = TargetTransformInfo;
  friend BaseT;

  const KlaussCPUSubtarget *ST;
  const KlaussCPUTargetLowering *TLI;

  const KlaussCPUSubtarget *getST() const { return ST; }
  const KlaussCPUTargetLowering *getTLI() const { return TLI; }

public:
  explicit KlaussCPUTTIImpl(const KlaussCPUTargetMachine *TM, const Function &F)
      : BaseT(TM, F.getDataLayout()), ST(TM->getSubtargetImpl(F)),
        TLI(ST->getTargetLowering()) {}

  void getUnrollingPreferences(Loop *L, ScalarEvolution &SE,
                               TTI::UnrollingPreferences &UP,
                               OptimizationRemarkEmitter *ORE) const override;

  void getPeelingPreferences(Loop *L, ScalarEvolution &SE,
                             TTI::PeelingPreferences &PP) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUTARGETTRANSFORMINFO_H
