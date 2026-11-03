//===-- KlaussCPULoopTripCount.cpp - Record a small loop's trip count -----===//
//
// Block placement aligns a loop head to the 16-byte fetch line when the
// fall-through edge into it is cold against the block (a trip count of five
// or more by the static estimate, which has no idea of a constant trip
// count: every loop looks like it runs about thirty-two times).  When the
// preheader falls into the head the alignment NOPs run once per entry, and
// a loop that runs twice per entry (a 2-iteration copy loop in Dhrystone's
// dhry_run) loses more to them than the alignment gains.  The machine
// passes cannot see the trip count, so this pass, after loop strength
// reduction, asks ScalarEvolution for it and records an upper bound
// (getSmallConstantMaxTripCount, iterations per entry) in the loop's
// metadata as !{!"klausscpu.loop.est_trips", i32 N}; KlaussCPULoopAlign
// drops the alignment of a loop that is entered by fall-through and runs at
// most -klausscpu-fall-through-align-max-trips times per entry.
//
// A loop that still contains a call to its own function is the other case: a
// recursion that tail-call elimination turned into a loop for one of its
// calls (fib(n - 1) + fib(n - 2), quicksort's two halves).  Such a loop is
// entered once per call and runs about as often as the recursion branches,
// about one iteration per entry (perf_baseline's calls_fib: 1.3); a
// tail-recursive list walk has no call left in its body.  It gets the
// estimate 1.
//
// -mllvm -klausscpu-loop-trip-count=false disables it.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/LoopUtils.h"

// Not the option name below: opt registers every pass name as an option.
#define DEBUG_TYPE "klausscpu-trip-count-md"
#define PASS_NAME "KlaussCPU loop trip count metadata"

namespace llvm {
void initializeKlaussCPULoopTripCountPass(PassRegistry &);
FunctionPass *createKlaussCPULoopTripCountPass();
} // namespace llvm

using namespace llvm;

// Read back by KlaussCPULoopAlign.cpp.
static const char *const EstTripsMD = "klausscpu.loop.est_trips";

static cl::opt<bool> EnableLoopTripCount(
    "klausscpu-loop-trip-count", cl::Hidden, cl::init(true),
    cl::desc("KlaussCPU: record a loop's constant trip count bound in its "
             "metadata, for the loop-head alignment decision"));

namespace {

class KlaussCPULoopTripCount : public FunctionPass {
public:
  static char ID;
  KlaussCPULoopTripCount() : FunctionPass(ID) {}
  bool runOnFunction(Function &F) override;
  StringRef getPassName() const override { return PASS_NAME; }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<ScalarEvolutionWrapperPass>();
    AU.addPreserved<LoopInfoWrapperPass>();
    AU.addPreserved<ScalarEvolutionWrapperPass>();
    AU.addPreserved<DominatorTreeWrapperPass>();
  }
};

} // end anonymous namespace

char KlaussCPULoopTripCount::ID = 0;

INITIALIZE_PASS_BEGIN(KlaussCPULoopTripCount, DEBUG_TYPE, PASS_NAME, false,
                      false)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_END(KlaussCPULoopTripCount, DEBUG_TYPE, PASS_NAME, false, false)

// A direct call to F itself in one of L's blocks (not in an inner loop's: the
// outer loop around it is the one that runs many times per entry).
static bool callsItself(const Loop &L, const Function &F) {
  for (const BasicBlock *BB : L.blocks())
    for (const Instruction &I : *BB)
      if (const auto *CB = dyn_cast<CallBase>(&I))
        if (CB->getCalledFunction() == &F)
          return true;
  return false;
}

bool KlaussCPULoopTripCount::runOnFunction(Function &F) {
  if (!EnableLoopTripCount || skipFunction(F))
    return false;
  LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  ScalarEvolution &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  bool Changed = false;
  for (Loop *L : LI.getLoopsInPreorder()) {
    // Only a loop whose latch is a single block with its own terminator (the
    // loop ID lives there).
    if (!L->getLoopLatch())
      continue;
    unsigned TC = SE.getSmallConstantMaxTripCount(L);
    if (callsItself(*L, F))
      TC = 1; // the bound of a counting loop (fib: 2^31) is no estimate
    if (!TC)
      continue; // unknown, or too large for 32 bits
    addStringMetadataToLoop(L, EstTripsMD, TC);
    Changed = true;
  }
  return Changed;
}

FunctionPass *llvm::createKlaussCPULoopTripCountPass() {
  return new KlaussCPULoopTripCount();
}
