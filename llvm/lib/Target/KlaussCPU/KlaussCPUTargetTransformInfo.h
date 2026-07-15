//===-- KlaussCPUTargetTransformInfo.h - KlaussCPU TTI ---------*- C++ -*-===//
//
// KlaussCPU LLVM backend — target-specific TargetTransformInfo.
//
// The generic (BasicTTI) cost model assumes a conventional load/store machine.
// KlaussCPU's tuning tracks the microarchitecture, which changed with M7/M8:
//
//   Pre-M7 the core was fetch-bound (59-75% of cycles in the fetch FSM, single
//   16-byte IFB line) so every extra instruction word cost cycles, and this hook
//   disabled all body-duplicating unrolling.
//
//   M7's fill-through I-cache collapsed IF_MISS (board CPI -22..-44%) and exposed
//   DATA (GPR RAW) latency as the next bottleneck.  M8 attacks that with a
//   latency-aware scheduling model (KlaussCPUSchedule.td) plus MODERATE unrolling
//   here: partial + runtime unrolling (capped at MaxCount=4, well inside the 8 KB
//   I-cache) hands the scheduler independent iterations to fill the 2-cycle
//   dependent-chain gaps.  Peeling stays off (it duplicates the body for little
//   gain on these kernels).
//
// getUnrollingPreferences is tied to the scheduling model: with `-mcpu=no-sched`
// (NoSchedModel, the M8 A/B baseline) there is no scheduler to fill the gaps, so
// it reverts to the conservative pre-M8 preferences.
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
