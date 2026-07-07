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
  // KlaussCPU is fetch-bound (see the header): duplicating a loop body adds
  // instruction words to fetch every iteration, which the single-line IFB and
  // the fetch FSM turn into extra cycles.  Disable the body-duplicating unroll
  // modes; keep the target-independent full-unroll path for tiny constant-trip
  // loops (it deletes the back-branch without emitting a remainder loop).
  UP.Partial = false;          // no partial unrolling of larger loops
  UP.Runtime = false;          // no unrolling when the trip count is unknown
  UP.UpperBound = false;       // do not unroll using a trip-count upper bound
  UP.UnrollAndJam = false;     // no unroll-and-jam (also body duplication)
  UP.PartialThreshold = 0;     // belt-and-braces: zero budget for partial unroll

  // Let a constant, provably-tiny loop still fully unroll, but keep the budget
  // small so we never trade a large fetch-count increase for one fewer branch.
  UP.MaxCount = 1;             // never emit more than one copy via count-based unroll
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
