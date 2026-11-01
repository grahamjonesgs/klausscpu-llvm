//===-- KlaussCPUPostIncLoopCmp.cpp - Test the updated loop counter -------===//
//
// A loop whose exit test compares the counter before its update,
//
//     %i      = phi [ Init, %preheader ], [ %i.next, %latch ]
//     ...
//     %i.next = add nsw %i, K
//     %c      = icmp slt %i, N - K            (InstCombine's canonical form
//     br %c, %header, %exit                     of icmp slt %i.next, N)
//
// is `cmp rI, N-K; ...; addi rI, rI, K; jmpcc`: the update sits between the
// compare and the branch (PHI elimination copies %i.next into %i's register
// there), so KlaussCPUFuseCmpBr cannot fuse the pair (the compare operand is
// redefined before the branch) and the loop takes a CMPRV, a copy and a
// JMPcc instead of one FBR.  Comparing the updated counter,
//
//     %c      = icmp slt %i.next, N
//
// is the same predicate when the add does not wrap (nsw for a signed
// predicate, nuw for an unsigned one) and N - K + K does not wrap either,
// and loop strength reduction then keeps the post-increment compare
// (`addi; fbr`).  Without the flag the rewrite is not exact, so a loop
// whose add carries none is left alone.
//
// The rewrite puts the update directly ahead of the branch that waits for it
// (an ALU result is ready two instructions later), where the old form had
// the compare, a copy and the jump in between.  That costs a cycle when
// nothing else fills the gap, as in a loop bound by a multiply or divide
// recurrence (`s = s * 5 + i`: 13 -> 14 cycles per iteration on the board,
// whose MUL shadow the old form's extra instructions fill); a loop bound by
// loads, stores or plain ALU work gains the instructions it no longer runs
// (3-9%).  A loop with a MUL/DIV in a recurrence of another header PHI is
// left alone.
//
// Runs from -O1 up, before loop strength reduction (which drops the add's
// flags when it rebuilds the increment; addIRPasses).  Only a compare of a
// header PHI with a constant, in the block of its latch-edge increment, ahead
// of which the add stands.  -mllvm -klausscpu-postinc-loop-cmp=false disables
// it.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "klausscpu-postinc-cmp"
#define PASS_NAME "KlaussCPU loop exit test on the updated counter"

namespace llvm {
void initializeKlaussCPUPostIncLoopCmpPass(PassRegistry &);
FunctionPass *createKlaussCPUPostIncLoopCmpPass();
} // namespace llvm

using namespace llvm;

static cl::opt<bool> EnablePostIncLoopCmp(
    "klausscpu-postinc-loop-cmp", cl::Hidden, cl::init(true),
    cl::desc("KlaussCPU: compare a loop's updated counter with the limit "
             "(not the old counter with limit - step) so the compare and "
             "the branch fuse"));

namespace {

class KlaussCPUPostIncLoopCmp : public FunctionPass {
public:
  static char ID;
  KlaussCPUPostIncLoopCmp() : FunctionPass(ID) {}
  bool runOnFunction(Function &F) override;
  StringRef getPassName() const override { return PASS_NAME; }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addPreserved<LoopInfoWrapperPass>();
    AU.addPreserved<DominatorTreeWrapperPass>();
  }
};

} // end anonymous namespace

char KlaussCPUPostIncLoopCmp::ID = 0;

INITIALIZE_PASS_BEGIN(KlaussCPUPostIncLoopCmp, DEBUG_TYPE, PASS_NAME, false,
                      false)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_END(KlaussCPUPostIncLoopCmp, DEBUG_TYPE, PASS_NAME, false,
                    false)

// Does a recurrence of one of L's header PHIs other than IV (a value of the
// iteration that feeds the next one) pass through a multiply or divide?
static bool hasLongRecurrence(const Loop &L, const PHINode *IV) {
  BasicBlock *Latch = L.getLoopLatch();
  for (const PHINode &P : L.getHeader()->phis()) {
    if (&P == IV)
      continue;
    auto *Next = dyn_cast<Instruction>(P.getIncomingValueForBlock(Latch));
    if (!Next || !L.contains(Next))
      continue;
    // Instructions that depend on P, and instructions Next depends on.
    SmallPtrSet<const Instruction *, 16> Fwd, Bwd;
    SmallVector<const Instruction *, 16> Work;
    for (const User *U : P.users())
      if (auto *UI = dyn_cast<Instruction>(U))
        if (L.contains(UI) && !isa<PHINode>(UI) && Fwd.insert(UI).second)
          Work.push_back(UI);
    while (!Work.empty()) {
      const Instruction *I = Work.pop_back_val();
      for (const User *U : I->users())
        if (auto *UI = dyn_cast<Instruction>(U))
          if (L.contains(UI) && !isa<PHINode>(UI) && Fwd.insert(UI).second)
            Work.push_back(UI);
    }
    Bwd.insert(Next);
    Work.push_back(Next);
    while (!Work.empty()) {
      const Instruction *I = Work.pop_back_val();
      for (const Value *Op : I->operands())
        if (auto *OI = dyn_cast<Instruction>(Op))
          if (L.contains(OI) && !isa<PHINode>(OI) && Bwd.insert(OI).second)
            Work.push_back(OI);
    }
    for (const Instruction *I : Fwd) {
      if (!Bwd.count(I))
        continue;
      switch (I->getOpcode()) {
      case Instruction::Mul:
      case Instruction::UDiv:
      case Instruction::SDiv:
      case Instruction::URem:
      case Instruction::SRem:
        return true;
      default:
        break;
      }
    }
  }
  return false;
}

static bool rewriteLoop(Loop &L) {
  BasicBlock *Latch = L.getLoopLatch();
  BasicBlock *Header = L.getHeader();
  if (!Latch)
    return false;
  auto *Br = dyn_cast<CondBrInst>(Latch->getTerminator());
  if (!Br)
    return false;
  // The latch's branch is an exit test: one edge is the back edge.
  if (Br->getSuccessor(0) != Header && Br->getSuccessor(1) != Header)
    return false;
  auto *Cmp = dyn_cast<ICmpInst>(Br->getCondition());
  if (!Cmp || Cmp->getParent() != Latch || !Cmp->hasOneUse() ||
      Cmp->isEquality())
    return false;
  auto *IV = dyn_cast<PHINode>(Cmp->getOperand(0));
  auto *Limit = dyn_cast<ConstantInt>(Cmp->getOperand(1));
  if (!IV || !Limit || IV->getParent() != Header ||
      !IV->getType()->isIntegerTy(64))
    return false;

  // The counter's update: %i.next = add %i, K, in the latch ahead of the
  // compare, with the flag that makes the rewrite exact.
  auto *Inc = dyn_cast<BinaryOperator>(IV->getIncomingValueForBlock(Latch));
  if (!Inc || Inc->getOpcode() != Instruction::Add || Inc->getParent() != Latch ||
      Inc->getOperand(0) != IV || !Inc->comesBefore(Cmp))
    return false;
  auto *Step = dyn_cast<ConstantInt>(Inc->getOperand(1));
  if (!Step || Step->isZero())
    return false;
  if (hasLongRecurrence(L, IV))
    return false;
  bool Signed = Cmp->isSigned();
  if (!(Signed ? Inc->hasNoSignedWrap() : Inc->hasNoUnsignedWrap()))
    return false;
  bool Ov = false;
  APInt NewLimit = Signed ? Limit->getValue().sadd_ov(Step->getValue(), Ov)
                          : Limit->getValue().uadd_ov(Step->getValue(), Ov);
  if (Ov)
    return false;

  LLVM_DEBUG(dbgs() << "KlaussCPU post-increment loop compare: "
                    << Latch->getName() << '\n');
  Cmp->setOperand(0, Inc);
  Cmp->setOperand(1, ConstantInt::get(Limit->getType(), NewLimit));
  return true;
}

bool KlaussCPUPostIncLoopCmp::runOnFunction(Function &F) {
  if (!EnablePostIncLoopCmp || skipFunction(F))
    return false;
  LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  bool Changed = false;
  for (Loop *L : LI.getLoopsInPreorder())
    Changed |= rewriteLoop(*L);
  return Changed;
}

FunctionPass *llvm::createKlaussCPUPostIncLoopCmpPass() {
  return new KlaussCPUPostIncLoopCmp();
}
