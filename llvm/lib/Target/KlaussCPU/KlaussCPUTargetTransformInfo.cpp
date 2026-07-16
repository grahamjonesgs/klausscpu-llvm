//===-- KlaussCPUTargetTransformInfo.cpp - KlaussCPU TTI -------------------===//
//
// KlaussCPU LLVM backend — target-specific TargetTransformInfo.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUTargetTransformInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "klausscputti"

static cl::opt<bool> EnableBranchDensityGate(
    "klausscpu-unroll-branch-density-gate", cl::Hidden, cl::init(true),
    cl::desc("Scale the M8 unroll budget down for branch-dense loop bodies "
             "(M8 spec §7.4).  =false restores the flat M8 budget for A/B."));

/// Count the control-flow splits in \p L's body that survive unrolling and land
/// in the final instruction stream.
///
/// The latch terminator is the loop's own back-edge test: unrolling folds N of
/// those into one, so it is not "internal" control flow and is excluded.  Every
/// other conditional branch (an if/else diamond, an early break) is duplicated
/// once per unrolled copy and keeps splitting the body into separate blocks.
///
/// Selects count as branches here.  KlaussCPU has no CMOV — `ISD::SELECT` is
/// Custom-lowered to a compare + branch + PHI — so at the machine level a select
/// splits the body exactly like a source-level `if`.  This is not a corner case:
/// SimplifyCFG speculates `if (c) x++;` into a select long before loop-unroll
/// runs, so counting only terminators reads a genuinely branch-dense body as
/// straight-line.  The `branchy` kernel is precisely that shape — three source
/// `if`s arrive here as 8 selects behind 1 latch branch, and become 9 compare/
/// branch pairs in the emitted code.
///
/// Loops with no unique latch are reported as maximally branchy — they are
/// irregular enough that the interleaving premise below does not hold either.
static unsigned countInternalBranches(const Loop *L) {
  const BasicBlock *Latch = L->getLoopLatch();
  if (!Latch)
    return ~0U;

  unsigned N = 0;
  for (const BasicBlock *BB : L->blocks()) {
    for (const Instruction &I : *BB)
      N += isa<SelectInst>(I);

    if (BB == Latch)
      continue;
    const Instruction *T = BB->getTerminator();
    if (const auto *BI = dyn_cast<BranchInst>(T))
      N += BI->isConditional();
    else if (isa<SwitchInst>(T))
      ++N;
  }
  return N;
}

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
  //
  // A/B switch (spec §7.1): the M8 unroll re-tune is tied to the scheduling model
  // so `-mcpu=no-sched` (NoSchedModel) reverts BOTH halves of M8 with one flag.
  // Without a latency model the scheduler cannot fill dependent-chain gaps, so the
  // independent iterations unrolling produces have nowhere to go and only grow the
  // instruction stream — fall back to the conservative pre-M8 preferences
  // (full-unroll of tiny constant-trip loops only; no body-duplicating unroll).
  if (!ST->getSchedModel().hasInstrSchedModel()) {
    UP.Partial = false;
    UP.Runtime = false;
    UP.UnrollAndJam = false;
    UP.OptSizeThreshold = 0;
    return;
  }

  UP.Partial   = true;         // partial-unroll larger loops
  UP.Runtime   = true;         // and unknown trip counts (memory/pointer loops)
  UP.UpperBound = true;        // may unroll using a trip-count upper bound
  UP.UnrollAndJam = false;     // still off — little upside, and it duplicates bodies
  UP.MaxCount  = 4;            // cap copies: keep the body within the I-cache
  UP.Threshold        = 200;   // full-unroll budget (~ a few hundred bytes of code)
  UP.PartialThreshold = 100;   // partial-unroll budget — stay < ~1 KB unrolled
  UP.OptSizeThreshold = 0;     // never grow code when optimising for size

  // Spec §7.4: back the budget off where fetch pressure outweighs the scheduling
  // win.  The budget above assumes the scheduler can interleave the unrolled
  // copies to fill the 2-cycle dependent-chain gaps.  That premise only holds
  // when the copies land in ONE basic block: the MachineScheduler builds its DAG
  // per scheduling region (block-local), and KlaussCPU has no CMOV — SELECT is
  // expanded to branches, so an if/else body can never be if-converted flat.
  // Every unrolled copy of a branch-dense body therefore keeps its own control
  // flow and sits in its own blocks, which the scheduler cannot interleave: we
  // pay the full fetch cost of the duplication and collect almost none of the
  // DATA win.
  //
  // Measured on the board (`branchy`): DATA 18.6% -> 13.1% but IF_MISS 52.2% ->
  // 55.1%, net +6.5% cycles.  The straight-line kernels (alu, muldiv-chain) win
  // under the flat budget, so gate on branch density rather than lowering it for
  // everyone.  Thresholds here are starting points — sweep them against IF_MISS
  // as §7.4 prescribes; `=false` restores the flat budget for a clean A/B.
  if (!EnableBranchDensityGate)
    return;

  unsigned Branches = countInternalBranches(L);
  if (Branches >= 3) {
    // Dense control flow: the most code to fetch and the least for the scheduler
    // to do with it.  Revert to pre-M8 behaviour — full-unroll of tiny
    // constant-trip loops only (UP.Threshold), no body duplication.
    UP.Partial = false;
    UP.Runtime = false;
    UP.PartialThreshold = 0;
  } else if (Branches >= 1) {
    // Some control flow: halve the budget.  A 2x unroll still amortizes the
    // back-edge test without doubling fetch pressure a second time.
    UP.MaxCount = 2;
    UP.PartialThreshold = 50;
  }

  LLVM_DEBUG(dbgs() << "KlaussCPU unroll: " << L->getHeader()->getName() << ": "
                    << Branches << " internal branches -> MaxCount="
                    << UP.MaxCount << " PartialThreshold=" << UP.PartialThreshold
                    << (UP.Partial ? "" : " (partial off)") << "\n");
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
