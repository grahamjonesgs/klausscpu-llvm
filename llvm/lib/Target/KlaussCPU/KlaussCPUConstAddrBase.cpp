//===-- KlaussCPUConstAddrBase.cpp - Shared base for constant addresses ---===//
//
// Instruction selection makes every load or store at a constant address,
// such as an MMIO register (`REG_UART_STATUS`, runtime mmio.h), one 2-word
// absolute access, LDABSn / STABSn / MEMREADR / MEMSETR (MODE=10, EA =
// imm32).  That is the cheapest form for one or two accesses, but a driver
// that touches the registers of one device several times pays 2 words each,
// where a base register would make each a 1-word short LDIDXn / STIDXn
// (simm8 scaled by the access size: +-128 bytes for a byte, +-1 KiB for a
// doubleword).  For example sd_cmd() polls SD_STATUS and writes SD_DATA,
// 8 bytes apart, a dozen times.
//
// This pass groups the absolute accesses of a function whose addresses are
// within short-form reach of one of them, the base, and when a group has at
// least -klausscpu-const-addr-base-min accesses (3: then the base's SETRU,
// 2 words, and one word per access are smaller than 2 words per access),
// sets the base in a register once, at the nearest common dominator of the
// accesses, and rewrites them to base + offset.  Later MachineLICM takes the
// base out of a loop.  The address of each access is unchanged: the base is
// zero-extended (SETR / SETRU) and the CPU uses bits [31:0] of base +
// offset (pipeline_core.sv:1001-1004).  Nor is the access: a naturally
// aligned constant (the AbsAddr patterns) gives the same EA alignment in
// both forms, and the indexed i64 forms are the force-aligned A=1 ones.
//
// A base live across a call would take a callee-saved register or be set
// again after the call, either of which can cost more than it saves.  So
// when a call lies between the nearest common dominator and an access, the
// group is split: if the dominator has accesses in front of its last call,
// those behind it and in the blocks below are tried again on their own;
// otherwise the accesses in the dominator itself form the runs between its
// calls, and the others are tried again by the dominator's child (in the
// dominator tree) whose subtree holds them.  Every part needs the minimum
// count again.  In a loop nest that holds a call, MachineLICM would take
// any base out of the loop, so it would live across the call anyway: there
// the accesses share one base set in front of the loop only when the nest
// holds at least -klausscpu-const-addr-base-loop-call-min of them (6: the
// base's SETRU and a callee-saved register's spill and reload are about 4
// words), and otherwise stay absolute.
//
// Runs on SSA machine code from -O1 up, before MachineLICM (addILPOpts),
// only with the short forms (otherwise nothing gets smaller).
// -mllvm -klausscpu-const-addr-base=false disables it.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUInstrInfo.h"
#include "KlaussCPUSubtarget.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include <map>

using namespace llvm;

// The pass name must differ from the option below (see KlaussCPUFuseCmpBr).
#define DEBUG_TYPE "klausscpu-const-addr"
#define PASS_NAME "KlaussCPU shared base for constant addresses"

STATISTIC(NumBases, "Base registers set for constant-address groups");
STATISTIC(NumRebased, "Absolute accesses rewritten to base + offset");

namespace llvm {
void initializeKlaussCPUConstAddrBasePass(PassRegistry &);
FunctionPass *createKlaussCPUConstAddrBasePass();
} // namespace llvm

static cl::opt<bool> EnableConstAddrBase(
    "klausscpu-const-addr-base", cl::Hidden, cl::init(true),
    cl::desc("Share a base register among constant-address loads and stores "
             "within short-form reach of each other"));

static cl::opt<unsigned> ConstAddrBaseMin(
    "klausscpu-const-addr-base-min", cl::Hidden, cl::init(3),
    cl::desc("Fewest constant-address accesses that share a base register"));

static cl::opt<unsigned> ConstAddrBaseLoopCallMin(
    "klausscpu-const-addr-base-loop-call-min", cl::Hidden, cl::init(6),
    cl::desc("Fewest constant-address accesses in a loop nest that holds a "
             "call that share a base register set in front of the loop"));

namespace {

struct Access {
  MachineInstr *MI;
  uint32_t Addr;
  unsigned Shift;       // log2 of the access size
  unsigned NewOpc;      // the base + offset twin
  unsigned CallsBefore; // calls in front of it in its block
};

class KlaussCPUConstAddrBase : public MachineFunctionPass {
public:
  static char ID;
  KlaussCPUConstAddrBase() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return PASS_NAME; }

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::IsSSA);
  }

  // addILPOpts: keeps the dominator tree and loop info for MachineLICM.
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<MachineDominatorTreeWrapperPass>();
    AU.addPreserved<MachineDominatorTreeWrapperPass>();
    AU.addRequired<MachineLoopInfoWrapperPass>();
    AU.addPreserved<MachineLoopInfoWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

private:
  const KlaussCPUInstrInfo *TII = nullptr;
  MachineRegisterInfo *MRI = nullptr;
  MachineDominatorTree *MDT = nullptr;
  MachineLoopInfo *MLI = nullptr;

  MachineLoop *getLoopWithCall(MachineBasicBlock *MBB) const;
  bool reachesWithoutCall(MachineBasicBlock *Dom,
                          MachineBasicBlock::iterator IP,
                          ArrayRef<const Access *> Group) const;
  bool place(ArrayRef<const Access *> Group, uint32_t Base);
  void rebase(ArrayRef<const Access *> Group, uint32_t Base,
              MachineBasicBlock &MBB, MachineBasicBlock::iterator IP);
};

} // end anonymous namespace

char KlaussCPUConstAddrBase::ID = 0;

INITIALIZE_PASS_BEGIN(KlaussCPUConstAddrBase, DEBUG_TYPE, PASS_NAME, false,
                      false)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_END(KlaussCPUConstAddrBase, DEBUG_TYPE, PASS_NAME, false,
                    false)

// The base + offset twin of an absolute access and the log2 of its size, or
// 0 for any other instruction.
static unsigned getIndexedTwin(unsigned Opc, unsigned &Shift) {
  switch (Opc) {
  case KlaussCPU::LDABS8:    Shift = 0; return KlaussCPU::LDIDX8;
  case KlaussCPU::LDABS8_S:  Shift = 0; return KlaussCPU::LDIDX8_S;
  case KlaussCPU::STABS8:    Shift = 0; return KlaussCPU::STIDX8;
  case KlaussCPU::LDABS16:   Shift = 1; return KlaussCPU::LDIDX16;
  case KlaussCPU::LDABS16_S: Shift = 1; return KlaussCPU::LDIDX16_S;
  case KlaussCPU::STABS16:   Shift = 1; return KlaussCPU::STIDX16;
  case KlaussCPU::LDABS32:   Shift = 2; return KlaussCPU::LDIDX32;
  case KlaussCPU::LDABS32_S: Shift = 2; return KlaussCPU::LDIDX32_S;
  case KlaussCPU::STABS32:   Shift = 2; return KlaussCPU::STIDX32;
  case KlaussCPU::MEMREADR:  Shift = 3; return KlaussCPU::LDIDX64;
  case KlaussCPU::MEMSETR:   Shift = 3; return KlaussCPU::STIDX64;
  default:                   return 0;
  }
}

// The short form's offset: a simm8 scaled by the access size.
static bool fitsShort(int64_t Off, unsigned Shift) {
  return (Off & ((int64_t(1) << Shift) - 1)) == 0 && isInt<8>(Off >> Shift);
}

// The first instruction of MBB that is an access of Group, or the first
// terminator.
static MachineBasicBlock::iterator
getInsertPoint(MachineBasicBlock &MBB, ArrayRef<const Access *> Group) {
  for (MachineInstr &MI : MBB)
    if (llvm::any_of(Group, [&](const Access *A) { return A->MI == &MI; }))
      return MI;
  return MBB.getFirstTerminator();
}

// The outermost loop around MBB if that loop nest holds a call, or null.
// MachineLICM takes a base set anywhere in such a nest out of it (SETR /
// SETRU depend on nothing), and the base then lives across the call.
MachineLoop *
KlaussCPUConstAddrBase::getLoopWithCall(MachineBasicBlock *MBB) const {
  MachineLoop *L = MLI->getLoopFor(MBB);
  if (!L)
    return nullptr;
  L = L->getOutermostLoop();
  for (MachineBasicBlock *LoopMBB : L->blocks())
    for (const MachineInstr &MI : *LoopMBB)
      if (MI.isCall())
        return L;
  return nullptr;
}

// Whether no call lies between IP in Dom, which dominates every access of
// Group, and any of the accesses: none in Dom after IP, nor in a block on a
// path from Dom to an access.  (A call in Dom before IP does not count:
// every path through it to an access passes IP, which sets the base anew.)
// Nor in a loop around Dom (getLoopWithCall).
bool KlaussCPUConstAddrBase::reachesWithoutCall(
    MachineBasicBlock *Dom, MachineBasicBlock::iterator IP,
    ArrayRef<const Access *> Group) const {
  if (getLoopWithCall(Dom))
    return false;
  SmallPtrSet<MachineBasicBlock *, 16> Region;
  SmallVector<MachineBasicBlock *, 16> Worklist;
  for (const Access *A : Group)
    if (Region.insert(A->MI->getParent()).second)
      Worklist.push_back(A->MI->getParent());
  while (!Worklist.empty()) {
    MachineBasicBlock *MBB = Worklist.pop_back_val();
    if (MBB == Dom)
      continue;
    for (MachineBasicBlock *Pred : MBB->predecessors())
      if (MDT->dominates(Dom, Pred) && Region.insert(Pred).second)
        Worklist.push_back(Pred);
  }
  for (MachineBasicBlock *MBB : Region)
    for (const MachineInstr &MI :
         make_range(MBB == Dom ? IP : MBB->begin(), MBB->end()))
      if (MI.isCall())
        return false;
  return true;
}

// Rebase the accesses of Group, all within short-form reach of Base, if
// there are enough of them, splitting it where a call is in the way.
bool KlaussCPUConstAddrBase::place(ArrayRef<const Access *> Group,
                                   uint32_t Base) {
  if (Group.size() < ConstAddrBaseMin)
    return false;
  MachineBasicBlock *Dom = Group.front()->MI->getParent();
  for (const Access *A : Group)
    Dom = MDT->findNearestCommonDominator(Dom, A->MI->getParent());
  MachineBasicBlock::iterator IP = getInsertPoint(*Dom, Group);
  if (reachesWithoutCall(Dom, IP, Group)) {
    rebase(Group, Base, *Dom, IP);
    return true;
  }

  // In a loop nest that holds a call, the base goes in front of the loop
  // (where MachineLICM would take it anyway) and lives across the calls, in
  // a callee-saved register or set again behind them by the register
  // allocator (SETR / SETRU are rematerialisable).  That only pays for
  // enough accesses in the nest; the others are tried again on their own.
  if (MachineLoop *L = getLoopWithCall(Dom)) {
    SmallVector<const Access *, 16> InLoop, Outside;
    for (const Access *A : Group)
      (L->contains(A->MI->getParent()) ? InLoop : Outside).push_back(A);
    MachineBasicBlock *Preheader = L->getLoopPreheader();
    if (Preheader && InLoop.size() >= ConstAddrBaseLoopCallMin) {
      rebase(InLoop, Base, *Preheader, Preheader->getFirstTerminator());
      place(Outside, Base);
      return true;
    }
  }

  // The accesses in Dom, by the number of calls in front of them, as runs
  // with no call in between.  None in a loop that holds a call (fewer than
  // the minimum above), since a base set between its calls would be taken
  // out of the loop too.
  bool Changed = false;
  auto PlaceRuns = [&](ArrayRef<const Access *> InDom) {
    if (getLoopWithCall(Dom))
      return;
    std::map<unsigned, SmallVector<const Access *, 8>> Runs;
    for (const Access *A : InDom)
      Runs[A->CallsBefore].push_back(A);
    for (auto &[Calls, Run] : Runs)
      if (Run.size() >= ConstAddrBaseMin) {
        rebase(Run, Base, *Dom, getInsertPoint(*Dom, Run));
        Changed = true;
      }
  };

  // First the accesses behind the last call in Dom together with those in
  // the blocks below it, which a base set behind that call may reach.
  unsigned DomCalls =
      llvm::count_if(*Dom, [](const MachineInstr &MI) { return MI.isCall(); });
  SmallVector<const Access *, 16> Before, Tail;
  for (const Access *A : Group)
    (A->MI->getParent() == Dom && A->CallsBefore < DomCalls ? Before : Tail)
        .push_back(A);
  if (!Before.empty() && Tail.size() >= ConstAddrBaseMin) {
    Changed |= place(Tail, Base);
    PlaceRuns(Before);
    return Changed;
  }

  // Otherwise the runs in Dom, and the others by the child of Dom whose
  // subtree holds them.
  SmallVector<const Access *, 16> InDom;
  MapVector<MachineBasicBlock *, SmallVector<const Access *, 8>> Subtrees;
  for (const Access *A : Group) {
    MachineBasicBlock *MBB = A->MI->getParent();
    if (MBB == Dom) {
      InDom.push_back(A);
      continue;
    }
    while (MDT->getNode(MBB)->getIDom()->getBlock() != Dom)
      MBB = MDT->getNode(MBB)->getIDom()->getBlock();
    Subtrees[MBB].push_back(A);
  }
  PlaceRuns(InDom);
  for (auto &[Child, Part] : Subtrees)
    Changed |= place(Part, Base);
  return Changed;
}

// Set Base in a new register at IP and rewrite each access of Group to it.
void KlaussCPUConstAddrBase::rebase(ArrayRef<const Access *> Group,
                                    uint32_t Base, MachineBasicBlock &MBB,
                                    MachineBasicBlock::iterator IP) {
  Register BaseReg = MRI->createVirtualRegister(&KlaussCPU::GPRRegClass);
  DebugLoc DL = IP != MBB.end() ? IP->getDebugLoc() : DebugLoc();
  if (isInt<32>(Base))
    BuildMI(MBB, IP, DL, TII->get(KlaussCPU::SETR), BaseReg).addImm(Base);
  else
    BuildMI(MBB, IP, DL, TII->get(KlaussCPU::SETRU), BaseReg).addImm(Base);
  ++NumBases;
  for (const Access *A : Group) {
    MachineInstr &MI = *A->MI;
    int64_t Off = static_cast<int64_t>(A->Addr) - Base;
    BuildMI(*MI.getParent(), MI, MI.getDebugLoc(), TII->get(A->NewOpc))
        .add(MI.getOperand(0))
        .addReg(BaseReg)
        .addImm(Off)
        .cloneMemRefs(MI)
        .setMIFlags(MI.getFlags());
    LLVM_DEBUG(dbgs() << "Rebased " << MI);
    MI.eraseFromParent();
    ++NumRebased;
  }
}

bool KlaussCPUConstAddrBase::runOnMachineFunction(MachineFunction &MF) {
  if (!EnableConstAddrBase || ConstAddrBaseMin == 0 ||
      skipFunction(MF.getFunction()))
    return false;
  const auto &STI = MF.getSubtarget<KlaussCPUSubtarget>();
  if (!STI.hasShortForms())
    return false;
  TII = STI.getInstrInfo();
  MRI = &MF.getRegInfo();
  MDT = &getAnalysis<MachineDominatorTreeWrapperPass>().getDomTree();
  MLI = &getAnalysis<MachineLoopInfoWrapperPass>().getLI();

  // The absolute accesses to a literal address (not a symbol).
  SmallVector<Access, 32> Accesses;
  for (MachineBasicBlock &MBB : MF) {
    unsigned Calls = 0;
    for (MachineInstr &MI : MBB) {
      Calls += MI.isCall();
      unsigned Shift;
      unsigned NewOpc = getIndexedTwin(MI.getOpcode(), Shift);
      if (NewOpc && MI.getOperand(1).isImm())
        Accesses.push_back(
            {&MI, static_cast<uint32_t>(MI.getOperand(1).getImm()), Shift,
             NewOpc, Calls});
    }
  }
  if (Accesses.size() < ConstAddrBaseMin)
    return false;

  // Greedily take the base that the most remaining accesses reach.
  SmallVector<const Access *, 32> Left;
  for (const Access &A : Accesses)
    Left.push_back(&A);
  bool Changed = false;
  while (Left.size() >= ConstAddrBaseMin) {
    uint32_t Best = 0;
    unsigned BestCount = 0;
    for (const Access *B : Left) {
      unsigned Count = 0;
      for (const Access *A : Left)
        Count += fitsShort(static_cast<int64_t>(A->Addr) - B->Addr, A->Shift);
      if (Count > BestCount || (Count == BestCount && B->Addr < Best)) {
        Best = B->Addr;
        BestCount = Count;
      }
    }
    if (BestCount < ConstAddrBaseMin)
      break;
    SmallVector<const Access *, 16> Group, Rest;
    for (const Access *A : Left)
      (fitsShort(static_cast<int64_t>(A->Addr) - Best, A->Shift) ? Group
                                                                 : Rest)
          .push_back(A);
    Left = std::move(Rest);

    Changed |= place(Group, Best);
  }
  return Changed;
}

FunctionPass *llvm::createKlaussCPUConstAddrBasePass() {
  return new KlaussCPUConstAddrBase();
}
