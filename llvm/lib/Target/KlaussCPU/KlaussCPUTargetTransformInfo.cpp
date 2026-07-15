//===-- KlaussCPUTargetTransformInfo.cpp - KlaussCPU TTI -------------------===//
//
// KlaussCPU LLVM backend — target-specific TargetTransformInfo.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUTargetTransformInfo.h"

using namespace llvm;

#define DEBUG_TYPE "klausscputti"

void KlaussCPUTTIImpl::getUnrollingPreferences(
    Loop *L, ScalarEvolution &SE, TTI::UnrollingPreferences &UP,
    OptimizationRemarkEmitter *ORE) const {
  // M8 re-tune.  Pre-M7 the core was fetch-bound (IF_MISS 33-73% of cycles) so
  // duplicating a loop body just added fetch cycles, and this hook disabled all
  // unrolling.  M7's 1-cycle-hit I-cache collapsed IF_MISS (board: CPI -22..-44%),
  // and the exposed bottleneck is now DATA (GPR RAW) latency (18-61% of cycles).
  // MODERATE unrolling helps that: it amortizes loop overhead and, with the
  // latency-aware sched model (KlaussCPUSchedule.td), gives the scheduler
  // independent iterations to fill the 2-cycle dependent-chain gaps with.
  //
  // Guardrail: the I-cache is 8 KB (512 x 16 B lines) — keep the unrolled body
  // well inside it, or conflict misses re-introduce the fetch stalls we just
  // removed.  The thresholds below cap the body at roughly a few hundred bytes.
  UP.Partial   = true;         // partial-unroll larger loops
  UP.Runtime   = true;         // and unknown trip counts (memory/pointer loops)
  UP.UpperBound = true;        // may unroll using a trip-count upper bound
  UP.UnrollAndJam = false;     // still off — little upside, and it duplicates bodies
  UP.MaxCount  = 4;            // cap copies: keep the body within the I-cache
  UP.Threshold        = 200;   // full-unroll budget (~ a few hundred bytes of code)
  UP.PartialThreshold = 100;   // partial-unroll budget — stay < ~1 KB unrolled
  UP.OptSizeThreshold = 0;     // never grow code when optimising for size
}

void KlaussCPUTTIImpl::getPeelingPreferences(
    Loop *L, ScalarEvolution &SE, TTI::PeelingPreferences &PP) const {
  // Peeling duplicates the loop body (peeled iterations + rotated loop), which
  // is the same fetch-count penalty as partial unrolling on a fetch-bound core.
  PP.PeelCount = 0;
  PP.AllowPeeling = false;
  PP.AllowLoopNestsPeeling = false;
  PP.PeelProfiledIterations = false;
}
