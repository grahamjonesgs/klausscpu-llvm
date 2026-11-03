//===-- KlaussCPULoopAlign.cpp - no alignment for run-once fall-through loops =//
//
// Block placement pads a loop's head to the 16-byte fetch line
// (-klausscpu-pref-loop-align) when the fall-through edge into it is cold
// against the head (a static estimate of five trips or more).  The NOPs
// execute whenever the head is entered by falling into it, once per entry (0
// to 3, 1.5 on average), so a loop that runs about once per entry loses more
// to them than the alignment saves.  Unaligned, a loop head cost 0, 1, 1 or 2
// cycles per iteration by its phase in the line (REPORT.md section 5 of the
// posted-store measurement), 1 on average: the two break even at two
// iterations per entry, and the board bore that out (Dhrystone's 2-iteration
// copy loop: 7% fewer instructions, +1% cycles, the code behind it shifting
// phase), so only a trip count of one is given up (the default of
// -klausscpu-fall-through-align-max-trips).  Entered by a branch, the padding
// is dead code and the alignment stays.  The trip count per entry comes from
// KlaussCPULoopTripCount's loop metadata.
//
// This was part of KlaussCPUFetchLayout, which also moved instructions and
// added NOPs to keep loops' 2- and 3-word instructions off line crossings.
// On klaussCPU stq that part measured neutral to slightly negative (Embench
// without it: +0.02% cycles at -O2, -0.17% at -O1, -Os unchanged; the 2-entry
// next-line buffer of fetch-bypass made a 2-word op cost ~1.25 cycles, not
// 2), and it was removed.  This rule alone still pays (Embench +0.11% without
// it at -O2).
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "klausscpu-loop-align"
#define PASS_NAME "KlaussCPU loop alignment of run-once fall-through loops"

namespace llvm {
void initializeKlaussCPULoopAlignPass(PassRegistry &);
FunctionPass *createKlaussCPULoopAlignPass();
} // namespace llvm

static cl::opt<unsigned> FallThroughMaxTrips(
    "klausscpu-fall-through-align-max-trips", cl::Hidden, cl::init(1),
    cl::desc("A loop that runs at most this many iterations per entry (a "
             "constant trip count, or a recursive loop; "
             "KlaussCPULoopTripCount) and is entered by falling into its "
             "head is not aligned: the NOPs would run on every entry "
             "(0 = always keep the alignment)"));

STATISTIC(NumUnaligned, "Short loops entered by fall-through left unaligned");

namespace {

class KlaussCPULoopAlign : public MachineFunctionPass {
public:
  static char ID;
  KlaussCPULoopAlign() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;
  StringRef getPassName() const override { return PASS_NAME; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    AU.addRequired<MachineLoopInfoWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

private:
  bool dropFallThroughAlign(MachineLoop *L);
};

} // end anonymous namespace

char KlaussCPULoopAlign::ID = 0;

INITIALIZE_PASS_BEGIN(KlaussCPULoopAlign, DEBUG_TYPE, PASS_NAME, false, false)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_END(KlaussCPULoopAlign, DEBUG_TYPE, PASS_NAME, false, false)

FunctionPass *llvm::createKlaussCPULoopAlignPass() {
  return new KlaussCPULoopAlign();
}

// The trip count (per entry) KlaussCPULoopTripCount recorded in the loop's
// metadata, or 0.
static unsigned loopEstTrips(const MachineLoop *L) {
  MDNode *LoopID = L->getLoopID();
  if (!LoopID)
    return 0;
  for (const MDOperand &Op : drop_begin(LoopID->operands())) {
    auto *MD = dyn_cast<MDNode>(Op);
    if (!MD || MD->getNumOperands() != 2)
      continue;
    auto *S = dyn_cast<MDString>(MD->getOperand(0));
    if (!S || S->getString() != "klausscpu.loop.est_trips")
      continue;
    if (auto *C = mdconst::dyn_extract_or_null<ConstantInt>(MD->getOperand(1)))
      return C->getZExtValue();
  }
  return 0;
}

bool KlaussCPULoopAlign::dropFallThroughAlign(MachineLoop *L) {
  unsigned Trips = loopEstTrips(L);
  if (!FallThroughMaxTrips || !Trips || Trips > FallThroughMaxTrips)
    return false;
  bool Changed = false;
  for (MachineBasicBlock *MBB : L->blocks()) {
    if (MBB->getAlignment() <= Align(4) || MBB == &MBB->getParent()->front())
      continue;
    MachineBasicBlock *Pred = &*std::prev(MBB->getIterator());
    if (L->contains(Pred) || Pred->getFallThrough() != MBB)
      continue;
    LLVM_DEBUG(dbgs() << "loop-align: no alignment for " << MBB->getName()
                      << ", at most " << Trips
                      << " trips per entry and entered by fall-through\n");
    MBB->setAlignment(Align(1));
    ++NumUnaligned;
    Changed = true;
  }
  return Changed;
}

bool KlaussCPULoopAlign::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;
  auto &MLI = getAnalysis<MachineLoopInfoWrapperPass>().getLI();
  bool Changed = false;
  for (MachineLoop *Top : MLI)
    for (MachineLoop *L : depth_first(Top))
      Changed |= dropFallThroughAlign(L);
  return Changed;
}
