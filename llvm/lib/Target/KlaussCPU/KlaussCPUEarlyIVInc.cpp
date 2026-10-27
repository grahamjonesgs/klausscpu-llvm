//===-- KlaussCPUEarlyIVInc.cpp - Increment a loop counter early ----------===//
//
// A single-block loop whose exit test reads the incremented counter
//
//     %i      = phi [ Init, %preheader ], [ %i.next, %loop ]
//     ...       uses of %i
//     %i.next = add %i, C
//     %done   = icmp eq/ne %i.next, N
//
// pins the increment below the last use of %i (the two share a register
// after coalescing) and the compare/branch directly behind it, where it
// waits for the increment (an ALU result is ready two instructions later,
// KlaussCPUSchedule.td).  When that last use ends a long dependency chain,
// as in perf_baseline's ALU kernel (`b += i` at the end of the body), no
// schedule hides the bubble.  Counting from Init - C instead,
//
//     %i.prev = phi [ Init - C, %preheader ], [ %i, %loop ]
//     %i      = add %i.prev, C          (first in the block)
//     ...       uses of %i
//     %done   = icmp eq/ne %i, N - C
//
// computes the same values with the same instructions, but the increment no
// longer has to follow the uses, so the schedulers can issue it early.
//
// Done only when nothing gets slower or bigger: Init and N are constants and
// so are Init - C and N - C (32-bit, and N - C in FBR_RI's simm4 when N is),
// the compare is EQ/NE (the rewrite is exact modulo 2^n), %i.next has no
// other use, and every other use of %i in the loop has another operand that
// is ready no earlier than the increment's result (so waiting for %i costs
// nothing).  A load address or a GEP of %i fails that test.
//
// Runs after loop strength reduction (addIRPasses), from -O1 up, for a CPU
// with a scheduling model (not -mcpu=no-sched, the A/B baseline without the
// schedulers this pass exists for); -mllvm -klausscpu-early-iv-inc=false
// disables it.  The counter keeps its debug location: %i's debug values move
// to the new add, with a copy as %i.prev + C ahead of it at the top of the
// block, and %i.next's become the new add + C.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUInstrInfo.h"
#include "KlaussCPUSubtarget.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/Local.h"

#define DEBUG_TYPE "klausscpu-iv-inc"
#define PASS_NAME "KlaussCPU early loop counter increment"

namespace llvm {
void initializeKlaussCPUEarlyIVIncPass(PassRegistry &);
FunctionPass *createKlaussCPUEarlyIVIncPass();
} // namespace llvm

using namespace llvm;
using namespace llvm::PatternMatch;

static cl::opt<bool> EnableEarlyIVInc(
    "klausscpu-early-iv-inc", cl::Hidden, cl::init(true),
    cl::desc("KlaussCPU: count a single-block loop from Init - C so its "
             "increment can be scheduled before the counter's uses"));

namespace {

// Issue distance from an instruction to a dependent one, read from the
// subtarget's scheduling model (KlaussCPUSchedule.td) through one
// representative instruction per class: ADDR (ALU), LDIDX64 (load), MULR,
// DIVR.  Casts, GEPs and PHIs count 0: they may fold away, and a low
// estimate only makes the rewrite less likely.
struct Latencies {
  unsigned ALU, Load, Mul, Div;

  explicit Latencies(const KlaussCPUSubtarget &ST) {
    const MCSchedModel &SM = ST.getSchedModel();
    const TargetInstrInfo &TII = *ST.getInstrInfo();
    auto Of = [&](unsigned Opc) {
      return static_cast<unsigned>(std::max(
          0, SM.computeInstrLatency(ST, TII.get(Opc).getSchedClass())));
    };
    ALU = Of(KlaussCPU::ADDR);
    Load = Of(KlaussCPU::LDIDX64);
    Mul = Of(KlaussCPU::MULR);
    Div = Of(KlaussCPU::DIVR);
  }

  unsigned of(const Instruction &I) const {
    if (isa<PHINode>(I) || isa<CastInst>(I) || isa<GetElementPtrInst>(I))
      return 0;
    if (isa<LoadInst>(I))
      return Load;
    switch (I.getOpcode()) {
    case Instruction::Mul:
      return Mul;
    case Instruction::UDiv:
    case Instruction::SDiv:
    case Instruction::URem:
    case Instruction::SRem:
      return Div;
    default:
      return ALU;
    }
  }
};

// A 32-bit constant: one SETR/SETRU, like the constants it replaces.
bool fitsImm32(const APInt &V) {
  return V.isSignedIntN(32) || V.isIntN(32);
}

class KlaussCPUEarlyIVInc : public FunctionPass {
public:
  static char ID;
  KlaussCPUEarlyIVInc() : FunctionPass(ID) {}
  bool runOnFunction(Function &F) override;
  StringRef getPassName() const override { return PASS_NAME; }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<TargetPassConfig>();
    AU.addPreserved<LoopInfoWrapperPass>();
    AU.addPreserved<DominatorTreeWrapperPass>();
  }

private:
  bool rewriteLoop(Loop &L, const Latencies &Lat);
};

} // end anonymous namespace

char KlaussCPUEarlyIVInc::ID = 0;

INITIALIZE_PASS_BEGIN(KlaussCPUEarlyIVInc, DEBUG_TYPE, PASS_NAME, false, false)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetPassConfig)
INITIALIZE_PASS_END(KlaussCPUEarlyIVInc, DEBUG_TYPE, PASS_NAME, false, false)

bool KlaussCPUEarlyIVInc::rewriteLoop(Loop &L, const Latencies &Lat) {
  BasicBlock *BB = L.getHeader();
  BasicBlock *Preheader = L.getLoopPreheader();
  if (L.getNumBlocks() != 1 || !Preheader)
    return false;

  // The exit test: br (icmp eq/ne %i.next, N), with N a constant.
  auto *Br = dyn_cast<CondBrInst>(BB->getTerminator());
  if (!Br)
    return false;
  auto *Cmp = dyn_cast<ICmpInst>(Br->getCondition());
  if (!Cmp || Cmp->getParent() != BB || !Cmp->hasOneUse() ||
      !Cmp->isEquality())
    return false;
  Value *Next;
  const APInt *N;
  if (!match(Cmp, m_c_ICmp(m_Value(Next), m_APInt(N))))
    return false;

  // %i.next = add %i, C with %i the loop's PHI; %i.next feeds only the PHI
  // and the compare.
  auto *Inc = dyn_cast<BinaryOperator>(Next);
  Value *IVVal;
  const APInt *C;
  if (!Inc || Inc->getParent() != BB || !Inc->hasNUses(2) ||
      !match(Inc, m_c_Add(m_Value(IVVal), m_APInt(C))) || C->isZero())
    return false;
  auto *IV = dyn_cast<PHINode>(IVVal);
  if (!IV || IV->getParent() != BB || IV->getNumIncomingValues() != 2 ||
      IV->getIncomingValueForBlock(BB) != Inc)
    return false;
  auto *Init = dyn_cast<ConstantInt>(IV->getIncomingValueForBlock(Preheader));
  if (!Init)
    return false;
  for (const User *U : Inc->users())
    if (U != IV && U != Cmp)
      return false;

  APInt NewInit = Init->getValue() - *C;
  APInt NewN = *N - *C;
  if (!fitsImm32(NewInit) || !fitsImm32(NewN) ||
      (N->isSignedIntN(4) && !NewN.isSignedIntN(4)))
    return false;

  // Earliest issue cycle of each instruction in the block, from the
  // dependencies inside one iteration (PHIs and values from outside: 0).
  DenseMap<const Instruction *, unsigned> Ready;
  auto ReadyAt = [&](const Instruction &I, const Value *Ignore) {
    unsigned Cycle = 0;
    for (const Value *Op : I.operands()) {
      auto *OpI = dyn_cast<Instruction>(Op);
      if (Op == Ignore || !OpI || OpI->getParent() != BB || isa<PHINode>(OpI))
        continue;
      Cycle = std::max(Cycle, Ready.lookup(OpI) + Lat.of(*OpI));
    }
    return Cycle;
  };
  for (const Instruction &I : *BB)
    if (!isa<PHINode>(I))
      Ready[&I] = ReadyAt(I, nullptr);

  // Every other use of %i in the block must wait for another operand at
  // least as long as it would wait for the increment issued first.  A use by
  // a PHI is a copy at the end of the block; uses outside the loop are after
  // it.
  bool HasUse = false;
  for (const User *U : IV->users()) {
    auto *UI = cast<Instruction>(U);
    if (UI == Inc || UI->getParent() != BB || isa<PHINode>(UI))
      continue;
    if (ReadyAt(*UI, IV) < Lat.of(*Inc))
      return false;
    HasUse = true;
  }
  if (!HasUse)
    return false;

  LLVM_DEBUG(dbgs() << "KlaussCPU early IV inc: " << BB->getName() << '\n');

  // %i.prev = phi [ Init - C, %preheader ], [ %i, %loop ]
  // %i      = add %i.prev, C
  // The add has no wrap flags: Init - C may wrap.
  Type *Ty = IV->getType();
  IRBuilder<> B(BB, BB->getFirstInsertionPt());
  PHINode *Prev = PHINode::Create(Ty, 2, IV->getName() + ".prev",
                                  BB->begin());
  auto *NewInc = cast<Instruction>(
      B.CreateAdd(Prev, ConstantInt::get(Ty, *C), IV->getName()));
  NewInc->setDebugLoc(Inc->getDebugLoc());
  Prev->addIncoming(ConstantInt::get(Ty, NewInit), Preheader);
  Prev->addIncoming(NewInc, BB);

  // icmp %i.next, N -> icmp %i, N - C
  unsigned IncOp = Cmp->getOperand(0) == Inc ? 0 : 1;
  Cmp->setOperand(IncOp, NewInc);
  Cmp->setOperand(1 - IncOp, ConstantInt::get(Ty, NewN));

  // Until the new add, %i is %i.prev + C: repeat %i's debug values at the
  // top of the block (where the old PHI's stood) in that form, so the
  // variable has a location before the schedulers' choice of slot for the
  // add, too.
  if (C->getSignificantBits() <= 64) {
    SmallVector<DbgVariableRecord *> DVRs;
    findDbgValues(IV, DVRs);
    for (DbgVariableRecord *DVR : DVRs) {
      if (DVR->hasArgList() ||
          DVR->getMarker()->MarkedInstr != NewInc->getNextNode())
        continue;
      DbgVariableRecord *Copy = DVR->clone();
      Copy->replaceVariableLocationOp(IV, Prev);
      Copy->setExpression(DIExpression::prepend(
          Copy->getExpression(), DIExpression::StackValue, C->getSExtValue()));
      BB->insertDbgRecordBefore(Copy, NewInc->getIterator());
    }
  }

  // %i's value is the new add's: replace it everywhere, debug values
  // included (the old increment now reads the new add and computes the next
  // iteration's value).  The old increment's debug values become the new
  // add + C; then drop the old PHI and increment, which only feed each other.
  IV->replaceAllUsesWith(NewInc);
  salvageDebugInfo(*Inc);
  IV->eraseFromParent();
  Inc->eraseFromParent();
  return true;
}

bool KlaussCPUEarlyIVInc::runOnFunction(Function &F) {
  if (!EnableEarlyIVInc || skipFunction(F))
    return false;
  const TargetMachine &TM =
      getAnalysis<TargetPassConfig>().getTM<TargetMachine>();
  const auto &ST =
      *static_cast<const KlaussCPUSubtarget *>(TM.getSubtargetImpl(F));
  if (!ST.getSchedModel().hasInstrSchedModel())
    return false;
  Latencies Lat(ST);
  LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  bool Changed = false;
  for (Loop *L : LI.getLoopsInPreorder())
    Changed |= rewriteLoop(*L, Lat);
  return Changed;
}

FunctionPass *llvm::createKlaussCPUEarlyIVIncPass() {
  return new KlaussCPUEarlyIVInc();
}
