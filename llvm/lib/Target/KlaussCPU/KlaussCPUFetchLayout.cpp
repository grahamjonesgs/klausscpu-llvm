//===-- KlaussCPUFetchLayout.cpp - keep wide instructions in one fetch line ===//
//
// The core (klaussCPU 8e6b8e5) fetches 16-byte lines.  A 1-word instruction
// never costs a bubble at a line boundary, but in a loop (board, the
// runtime's perf_kern):
//
//   - a 2- or 3-word instruction that crosses a line takes an extra cycle;
//   - a 2-word load or store takes one more cycle than its 1-word form unless
//     it starts a line, and a 2-word store that crosses a line one more.
//
// (On 3508f87 a 2-word instruction in the first line after a taken branch
// also cost one more; picolibc's sqrt lost 7.2% to two crossings there.)
//
// This pass runs last before the AsmPrinter, when the size of every
// instruction is known (getInstSizeInBytes agrees with the AsmPrinter's
// compressions, and the offsets inside a loop are fixed).  The position of an
// instruction within a line is only known relative to a 16-byte aligned
// point, so it works on loops whose first block is aligned (the loop-head
// alignment, -klausscpu-pref-loop-align): the offset of every instruction
// from that block is the sum of the sizes in front of it, rounded up at each
// aligned block.  For each such loop it scores the layout with a small
// in-order timing model (issue slots by size, the fetch penalties above, the
// load-use, ALU, MUL and data-port stalls of KlaussCPUSchedule.td),
// weighted by block frequency, and keeps the cheapest of
//
//   - moving one instruction up or down by at most MaxMove positions inside
//     its block, past instructions it is independent of (registers, FLAGS,
//     memory order, calls, SP, side effects), when that does not make the
//     model's stalls (without the fetch penalties) any worse, and
//   - when not optimising for size, one NOP in front of a wide instruction
//     (it costs a cycle per iteration, so it has to save more than one).
//
// A change is made only if the model's cost strictly drops.  The pass never
// moves a terminator, a CFI or debug instruction; debug instructions never
// influence a decision (so -g changes nothing); a debug instruction that
// reads a register the moved instruction writes is moved with it.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUInstrInfo.h"
#include "KlaussCPUSubtarget.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "klausscpu-fetch-layout"
#define PASS_NAME "KlaussCPU fetch-line layout of loops"

namespace llvm {
void initializeKlaussCPUFetchLayoutPass(PassRegistry &);
FunctionPass *createKlaussCPUFetchLayoutPass();
} // namespace llvm

static cl::opt<unsigned> FetchLayoutLevel(
    "klausscpu-fetch-line-layout", cl::Hidden, cl::init(2),
    cl::desc("Late loop layout against 16-byte fetch lines: 0 = off, 1 = move "
             "independent instructions, 2 = also one NOP in front of a wide "
             "instruction (not when optimising for size)"));

static cl::opt<unsigned> MaxMove(
    "klausscpu-fetch-layout-max-move", cl::Hidden, cl::init(4),
    cl::desc("Farthest an instruction may move inside its block"));

static cl::opt<unsigned> MaxLoopInstrs(
    "klausscpu-fetch-layout-max-instrs", cl::Hidden, cl::init(400),
    cl::desc("Largest loop (in instructions) the pass looks at"));

static cl::opt<unsigned> FallThroughMaxTrips(
    "klausscpu-fall-through-align-max-trips", cl::Hidden, cl::init(1),
    cl::desc("A loop that runs at most this many iterations per entry (a "
             "constant trip count, or a recursive loop; "
             "KlaussCPULoopTripCount) and is entered by falling into its "
             "head is not aligned: the NOPs would run on every entry "
             "(0 = always keep the alignment)"));

STATISTIC(NumUnaligned, "Short loops entered by fall-through left unaligned");
STATISTIC(NumMoves, "Instructions moved to avoid a fetch-line crossing");
STATISTIC(NumNops, "NOPs inserted to avoid a fetch-line crossing");

namespace {

// At most this many NOPs per function: KlaussCPUFuseCmpBr checks a fused
// branch's range before this pass runs and leaves 384 bytes of margin for it.
constexpr unsigned MaxFetchLayoutNops = 96;

class KlaussCPUFetchLayout : public MachineFunctionPass {
public:
  static char ID;
  KlaussCPUFetchLayout() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;
  StringRef getPassName() const override { return PASS_NAME; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<MachineLoopInfoWrapperPass>();
    AU.addRequired<MachineBlockFrequencyInfoWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::NoVRegs);
  }

private:
  const KlaussCPUInstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  bool AllowNop = false;
  // NOPs this function may still get (see MaxFetchLayoutNops).
  unsigned NopsLeft = 0;

  // One loop being laid out: the layout range from its first block to its
  // last, with the blocks whose cost counts (the loop's own, not an inner
  // loop's, which is handled on its own) and their weights.
  struct Region {
    SmallVector<MachineBasicBlock *, 16> Blocks; // layout range
    DenseMap<const MachineBasicBlock *, double> Weight;
  };

  bool dropFallThroughAlign(MachineLoop *L);
  bool processLoop(MachineLoop *L, MachineLoopInfo &MLI,
                   MachineBlockFrequencyInfo &MBFI);
  double cost(const Region &R, bool Fetch = true) const;
  bool independent(const MachineInstr &A, const MachineInstr &B) const;
};

} // end anonymous namespace

char KlaussCPUFetchLayout::ID = 0;

INITIALIZE_PASS_BEGIN(KlaussCPUFetchLayout, DEBUG_TYPE, PASS_NAME, false, false)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineBlockFrequencyInfoWrapperPass)
INITIALIZE_PASS_END(KlaussCPUFetchLayout, DEBUG_TYPE, PASS_NAME, false, false)

FunctionPass *llvm::createKlaussCPUFetchLayoutPass() {
  return new KlaussCPUFetchLayout();
}

// Instructions whose effects the register and memory checks do not describe,
// or that must stay where they are.
static bool isFixed(const MachineInstr &MI) {
  if (MI.isTerminator() || MI.isCall() || MI.isReturn() || MI.isBarrier() ||
      MI.isInlineAsm() || MI.isPosition() || MI.isCFIInstruction() ||
      MI.hasUnmodeledSideEffects() || MI.isMetaInstruction())
    return true;
  switch (MI.getOpcode()) {
  // The stack pointer is not in their operands (the return address, the
  // interrupt frame); anything else that names SP is caught below.
  case KlaussCPU::RET_I:
  case KlaussCPU::IRET_I:
  case KlaussCPU::LEAVERET_I:
  case KlaussCPU::PUSHV:
  case KlaussCPU::PUSHV64:
    return true;
  default:
    break;
  }
  return MI.getDesc().hasImplicitUseOfPhysReg(KlaussCPU::SP) ||
         MI.getDesc().hasImplicitDefOfPhysReg(KlaussCPU::SP);
}

bool KlaussCPUFetchLayout::independent(const MachineInstr &A,
                                       const MachineInstr &B) const {
  if (isFixed(A) || isFixed(B))
    return false;
  // Memory: two loads may swap; anything with a store or an ordered access
  // (volatile, atomic) keeps its order.
  if ((A.mayLoadOrStore() && B.mayLoadOrStore() &&
       (A.mayStore() || B.mayStore())) ||
      A.hasOrderedMemoryRef() || B.hasOrderedMemoryRef())
    return false;
  for (const MachineOperand &OA : A.operands()) {
    if (OA.isRegMask())
      return false;
    if (!OA.isReg() || !OA.getReg())
      continue;
    for (const MachineOperand &OB : B.operands()) {
      if (OB.isRegMask())
        return false;
      if (!OB.isReg() || !OB.getReg() || !TRI->regsOverlap(OA.getReg(), OB.getReg()))
        continue;
      if (OA.isUse() && OB.isUse())
        continue;
      // Two FLAGS writes that nobody reads (both dead) may swap.
      if (OA.getReg() == KlaussCPU::FLAGS && OA.isDef() && OB.isDef() &&
          OA.isDead() && OB.isDead())
        continue;
      return false;
    }
  }
  return true;
}

namespace {
// The timing model: cycles to run one block's instructions in order, from a
// state where everything is ready, at the given offset from an aligned
// point.  Slot counts, latencies and penalties are the board measurements of
// KlaussCPUSchedule.td and the runtime's perf_kern.
struct Timer {
  unsigned Cur = 0;
  unsigned LoadFree = 0;  // first cycle a load may issue (the data port)
  unsigned StoreFree = 0; // first cycle a store may issue
  SmallDenseMap<unsigned, unsigned, 16> Ready;
};
} // namespace

double KlaussCPUFetchLayout::cost(const Region &R, bool Fetch) const {
  double Total = 0;
  unsigned Off = 0;
  for (MachineBasicBlock *MBB : R.Blocks) {
    Off = alignTo(Off, MBB->getAlignment().value());
    auto WI = R.Weight.find(MBB);
    double W = WI == R.Weight.end() ? 0 : WI->second;
    Timer T;
    for (const MachineInstr &MI : *MBB) {
      unsigned Size = TII->getInstSizeInBytes(MI);
      if (!Size) {
        continue;
      }
      if (W == 0) {
        Off += Size;
        continue;
      }
      unsigned Words = Size / 4;
      if (MI.isCall()) {
        T.Cur += Words;
        T.Ready.clear();
        T.LoadFree = T.StoreFree = 0;
        Off += Size;
        continue;
      }
      // Fetch: a wide instruction costs a cycle per word, plus one more if
      // it crosses a line; a 2-word load or store one more unless it starts
      // a line, and a 2-word store one more again if it crosses one.
      bool Load = MI.mayLoad(), Store = !Load && MI.mayStore();
      unsigned Slots = Words, Extra = 0;
      if (Fetch && Words > 1) {
        bool Crosses = (Off & 15) + Size > 16;
        if (Crosses)
          ++Extra;
        if ((Load || Store) && (Off & 15))
          ++Extra;
        if (Store && Crosses)
          ++Extra;
      }
      unsigned Earliest = T.Cur;
      for (const MachineOperand &MO : MI.operands())
        if (MO.isReg() && MO.isUse() && MO.getReg()) {
          auto It = T.Ready.find(MO.getReg());
          if (It != T.Ready.end())
            Earliest = std::max(Earliest, It->second);
        }
      // A load holds the pipeline 3 cycles, one more directly behind another
      // load, and its result is ready 5 cycles after it issues.  A store
      // holds it 2; the next store may issue 3 cycles after it, the next
      // load 4.
      unsigned Lat = 2, Hold = 1;
      if (Load) {
        Earliest = std::max(Earliest, T.LoadFree);
        Hold = 3;
        Lat = 5;
        T.LoadFree = Earliest + 4;
      } else if (Store) {
        Earliest = std::max(Earliest, T.StoreFree);
        Hold = 2;
        T.StoreFree = Earliest + 3;
        T.LoadFree = std::max(T.LoadFree, Earliest + 4);
      } else if (MI.getOpcode() == KlaussCPU::MULR ||
                 MI.getOpcode() == KlaussCPU::MULV) {
        Hold = 4;
        Lat = 5;
      }
      Slots = std::max(Slots, Hold) + Extra;
      for (const MachineOperand &MO : MI.operands())
        if (MO.isReg() && MO.isDef() && MO.getReg())
          T.Ready[MO.getReg()] = Earliest + Lat;
      T.Cur = Earliest + Slots;
      Off += Size;
    }
    Total += W * T.Cur;
  }
  return Total;
}

namespace {
struct Action {
  MachineInstr *X = nullptr;
  int Dir = 0;        // +1 later, -1 earlier, 0 = NOP in front of X
  unsigned Steps = 0; // real instructions crossed
};
} // namespace

// Debug instructions in [A, B) that read a register MI writes.  They must stay
// on the same side of MI as before, so they move with it (after the final
// move only: scoring ignores debug instructions, which keeps -g from changing
// any decision).
static void collectDebugReadingDefOf(MachineBasicBlock::iterator A,
                                     MachineBasicBlock::iterator B,
                                     const MachineInstr &MI,
                                     SmallVectorImpl<MachineInstr *> &Out) {
  for (auto I = A; I != B; ++I) {
    if (!I->isDebugInstr())
      continue;
    bool Hit = false;
    for (const MachineOperand &DO : I->operands())
      if (DO.isReg() && DO.getReg())
        for (const MachineOperand &MO : MI.operands())
          if (MO.isReg() && MO.isDef() && MO.getReg() == DO.getReg())
            Hit = true;
    if (Hit)
      Out.push_back(&*I);
  }
}

// Apply an action (the instruction is not moved when it cannot: false).
// Undo information: the instruction that followed X.
static bool applyMove(const Action &A, MachineBasicBlock::iterator &Orig,
                      const KlaussCPUInstrInfo *TII,
                      function_ref<bool(const MachineInstr &,
                                        const MachineInstr &)>
                          Indep,
                      bool FixDebug = false) {
  MachineInstr &X = *A.X;
  MachineBasicBlock &MBB = *X.getParent();
  Orig = std::next(X.getIterator());
  if (A.Dir == 0) {
    BuildMI(MBB, X.getIterator(), X.getDebugLoc(), TII->get(KlaussCPU::NOP_I));
    return true;
  }
  auto It = X.getIterator();
  auto End = MBB.getFirstTerminator();
  MachineBasicBlock::iterator Target = It;
  unsigned Crossed = 0;
  if (A.Dir > 0) {
    for (auto J = std::next(It); J != End && Crossed < A.Steps; ++J) {
      if (J->isDebugInstr())
        continue;
      if (!Indep(X, *J))
        return false;
      ++Crossed;
      Target = std::next(J);
    }
    if (Crossed != A.Steps)
      return false;
    SmallVector<MachineInstr *, 2> Dbg;
    if (FixDebug)
      collectDebugReadingDefOf(std::next(It), Target, X, Dbg);
    MBB.splice(Target, &MBB, It);
    for (MachineInstr *D : Dbg) // they followed X: keep it so
      MBB.splice(Target, &MBB, D);
  } else {
    for (auto J = It; J != MBB.begin() && Crossed < A.Steps;) {
      --J;
      if (J->isDebugInstr())
        continue;
      if (!Indep(*J, X))
        return false;
      ++Crossed;
      Target = J;
    }
    if (Crossed != A.Steps)
      return false;
    SmallVector<MachineInstr *, 2> Dbg;
    if (FixDebug)
      collectDebugReadingDefOf(Target, It, X, Dbg);
    MBB.splice(Target, &MBB, It);
    for (MachineInstr *D : Dbg) // they preceded X: keep it so
      MBB.splice(It, &MBB, D);
  }
  return true;
}

static void undoMove(const Action &A, MachineBasicBlock::iterator Orig) {
  MachineInstr &X = *A.X;
  if (A.Dir == 0) {
    // The NOP is the instruction in front of X.
    std::prev(X.getIterator())->eraseFromParent();
    return;
  }
  X.getParent()->splice(Orig, X.getParent(), X.getIterator());
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

// Block placement pads a loop's head to the fetch line when the fall-through
// edge into it is cold against the head (a static estimate of five trips or
// more).  The NOPs execute whenever the head is entered by falling into it,
// once per entry (0 to 3, 1.5 on average), so a loop that runs about once per
// entry loses more to them than the alignment saves.  Unaligned, a loop head
// costs 0, 1, 1 or 2 cycles per iteration by its phase in the line (REPORT.md
// section 5 of the posted-store measurement), 1 on average: the two break
// even at two iterations per entry, and the board bears that out (Dhrystone's
// 2-iteration copy loop: 7% fewer instructions, +1% cycles, the code behind
// it shifting phase), so only a trip count of one is given up (the default of
// -klausscpu-fall-through-align-max-trips).  Entered by a branch, the padding
// is dead code and the alignment stays.
bool KlaussCPUFetchLayout::dropFallThroughAlign(MachineLoop *L) {
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
    LLVM_DEBUG(dbgs() << "fetch-layout: no alignment for " << MBB->getName()
                      << ", at most " << Trips
                      << " trips per entry and entered by fall-through\n");
    MBB->setAlignment(Align(1));
    ++NumUnaligned;
    Changed = true;
  }
  return Changed;
}

bool KlaussCPUFetchLayout::processLoop(MachineLoop *L, MachineLoopInfo &MLI,
                                       MachineBlockFrequencyInfo &MBFI) {
  MachineFunction &MF = *L->getHeader()->getParent();
  SmallVector<MachineBasicBlock *, 32> Order;
  for (MachineBasicBlock &MBB : MF)
    Order.push_back(&MBB);
  int First = -1, Last = -1;
  for (int I = 0, E = Order.size(); I != E; ++I)
    if (L->contains(Order[I])) {
      if (First < 0)
        First = I;
      Last = I;
    }
  if (First < 0 || Order[First]->getAlignment() < Align(16))
    return false;

  Region R;
  unsigned NumInstrs = 0;
  uint64_t HF = MBFI.getBlockFreq(L->getHeader()).getFrequency();
  for (int I = First; I <= Last; ++I) {
    MachineBasicBlock *MBB = Order[I];
    R.Blocks.push_back(MBB);
    for (const MachineInstr &MI : *MBB) {
      if (MI.isInlineAsm())
        return false; // size unknown
      if (!MI.isMetaInstruction())
        ++NumInstrs;
    }
    if (MLI.getLoopFor(MBB) == L) {
      double W = HF ? (double)MBFI.getBlockFreq(MBB).getFrequency() / HF : 1.0;
      R.Weight[MBB] = std::min(W, 8.0);
    }
  }
  if (NumInstrs > MaxLoopInstrs)
    return false;

  auto Indep = [this](const MachineInstr &A, const MachineInstr &B) {
    return independent(A, B);
  };
  bool Changed = false;
  for (unsigned Round = 0; Round != 12; ++Round) {
    double Base = cost(R), Best = Base - 1e-6;
    double BaseT = cost(R, /*Fetch=*/false);
    Action BestA;
    for (MachineBasicBlock *MBB : R.Blocks) {
      if (!R.Weight.count(MBB))
        continue;
      SmallVector<MachineInstr *, 64> Instrs;
      for (MachineInstr &MI : *MBB)
        if (!MI.isDebugInstr() && !isFixed(MI))
          Instrs.push_back(&MI);
      for (MachineInstr *X : Instrs) {
        for (int Dir : {1, -1})
          for (unsigned K = 1; K <= MaxMove; ++K) {
            Action A{X, Dir, K};
            MachineBasicBlock::iterator Orig;
            if (!applyMove(A, Orig, TII, Indep))
              break; // farther moves cross the same blocker
            double C = cost(R);
            // The scheduler chose the order for the data hazards: a move
            // may only buy fetch cycles, never cost any.
            double CT = cost(R, /*Fetch=*/false);
            undoMove(A, Orig);
            if (C < Best && CT <= BaseT + 1e-6 &&
                C - CT < Base - BaseT - 1e-6) {
              Best = C;
              BestA = A;
            }
          }
        if (AllowNop && NopsLeft && TII->getInstSizeInBytes(*X) >= 8) {
          Action A{X, 0, 0};
          MachineBasicBlock::iterator Orig;
          applyMove(A, Orig, TII, Indep);
          double C = cost(R);
          undoMove(A, Orig);
          if (C < Best) {
            Best = C;
            BestA = A;
          }
        }
      }
    }
    if (!BestA.X)
      break;
    MachineBasicBlock::iterator Orig;
    bool Ok = applyMove(BestA, Orig, TII, Indep, /*FixDebug=*/true);
    (void)Ok;
    assert(Ok && "a move that scored must apply");
    LLVM_DEBUG(dbgs() << "fetch-layout: " << (BestA.Dir ? "move " : "nop before ")
                      << *BestA.X << "  cost " << Base << " -> " << Best << "\n");
    if (BestA.Dir)
      ++NumMoves;
    else {
      ++NumNops;
      --NopsLeft;
    }
    Changed = true;
  }
  return Changed;
}

bool KlaussCPUFetchLayout::runOnMachineFunction(MachineFunction &MF) {
  if (!FetchLayoutLevel || skipFunction(MF.getFunction()))
    return false;
  TII = static_cast<const KlaussCPUInstrInfo *>(MF.getSubtarget().getInstrInfo());
  TRI = MF.getSubtarget().getRegisterInfo();
  AllowNop = FetchLayoutLevel >= 2 && !MF.getFunction().hasOptSize();
  NopsLeft = MaxFetchLayoutNops;
  auto &MLI = getAnalysis<MachineLoopInfoWrapperPass>().getLI();
  auto &MBFI = getAnalysis<MachineBlockFrequencyInfoWrapperPass>().getMBFI();
  bool Changed = false;
  SmallVector<MachineLoop *, 8> Loops;
  for (MachineLoop *Top : MLI)
    for (MachineLoop *L : post_order(Top))
      Loops.push_back(L);
  // Before the layout: a loop left unaligned is not laid out against the
  // fetch lines (their phase is unknown).
  for (MachineLoop *L : Loops)
    Changed |= dropFallThroughAlign(L);
  for (MachineLoop *L : Loops)
    Changed |= processLoop(L, MLI, MBFI);
  return Changed;
}
