//===-- KlaussCPUJumpTableHoist.cpp - Jump-table address into the header --===//
//
// A jump-table dispatch (LowerBR_JT) is two blocks: the switch's range check
// in the header, and in the dispatch block behind it
//
//     shlv      %off, %idx, 2           ; entry offset
//     leapc     %base, .LJTI0_0         ; table base (setru when static)
//     ldidx32r  %tgt, %base, %off       ; absolute 32-bit target
//     jmpr      %tgt
//
// Everything there is a chain: the table base is read by the very next
// instruction (1 bubble) and the target too (2 bubbles behind the load), and
// the post-RA scheduler, which works within one block, has nothing to put in
// between.  The offset and the base depend only on the header's index, so
// this pass moves them into the header, in front of its range check: there
// they fill the bubbles between the index computation and the compare, and
// the dispatch block is the load and the JMPR.  On the default path they
// are two dead single-cycle instructions.  Measured on the board, a 6- to
// 20-case switch dispatch takes 1.6 to 1.8 cycles less.  (The pre-RA
// scheduler keeps the range check's compare next to its branch, see
// KlaussCPUTargetMachine::createMachineScheduler; otherwise it would sink the
// base below the compare, and the base could take the compare operand's
// register and stop KlaussCPUFuseCmpBr.)
//
// Runs on SSA machine code before register allocation, after MachineSink
// (which would sink the two straight back) and after MachineLICM (which may
// already have taken the base out of a loop; then only the offset moves).
// Only side-effect-free instructions move (with what they read from the
// dispatch block itself, such as a COPY of the index, up to three each),
// whose register operands are all virtual and available at the insertion
// point, and only into the dispatch block's single predecessor.  An instruction that clobbers FLAGS (SHLV
// writes them, dead) goes in front of the compare that sets the flags the
// header's branch reads.
//
// -mllvm -klausscpu-hoist-jt-addr=false disables it.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUInstrInfo.h"
#include "KlaussCPUSubtarget.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/PseudoSourceValue.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

// The pass name must differ from the option below (see KlaussCPUFuseCmpBr).
#define DEBUG_TYPE "klausscpu-jump-table-hoist"
#define PASS_NAME "KlaussCPU jump-table address hoisting"

namespace llvm {
void initializeKlaussCPUJumpTableHoistPass(PassRegistry &);
FunctionPass *createKlaussCPUJumpTableHoistPass();
} // namespace llvm

static cl::opt<bool> EnableJTHoist(
    "klausscpu-hoist-jt-addr", cl::Hidden, cl::init(true),
    cl::desc("Compute a jump table's base and entry offset in the switch's "
             "range-check block"));

namespace {

class KlaussCPUJumpTableHoist : public MachineFunctionPass {
public:
  static char ID;
  KlaussCPUJumpTableHoist() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return PASS_NAME; }

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::IsSSA);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // end anonymous namespace

char KlaussCPUJumpTableHoist::ID = 0;

INITIALIZE_PASS(KlaussCPUJumpTableHoist, DEBUG_TYPE, PASS_NAME, false, false)

// The jump-table load feeding Br, a JMPR_R in the same block, or null.
static MachineInstr *getJumpTableLoad(MachineInstr &Br,
                                      const MachineRegisterInfo &MRI) {
  Register Tgt = Br.getOperand(0).getReg();
  if (!Tgt.isVirtual())
    return nullptr;
  MachineInstr *Load = MRI.getUniqueVRegDef(Tgt);
  if (!Load || Load->getParent() != Br.getParent() ||
      Load->getOpcode() != KlaussCPU::LDIDX32R || !Load->hasOneMemOperand())
    return nullptr;
  const PseudoSourceValue *PSV =
      (*Load->memoperands_begin())->getPseudoValue();
  if (!PSV || PSV->kind() != PseudoSourceValue::JumpTable)
    return nullptr;
  return Load;
}

// MI computes from virtual registers only, without side effects, and writes
// nothing but its result and a dead FLAGS.
static bool isHoistable(const MachineInstr &MI) {
  bool SawStore = true; // rejects loads as well
  if (MI.isPHI() || !MI.isSafeToMove(SawStore) || MI.getNumExplicitDefs() != 1)
    return false;
  for (const MachineOperand &MO : MI.operands()) {
    if (!MO.isReg() || !MO.getReg())
      continue;
    if (MO.getReg().isVirtual()) {
      if (MO.isDef() && &MO != &MI.getOperand(0))
        return false;
      continue;
    }
    if (!(MO.isDef() && MO.isImplicit() && MO.isDead() &&
          MO.getReg() == KlaussCPU::FLAGS))
      return false;
  }
  return true;
}

bool KlaussCPUJumpTableHoist::runOnMachineFunction(MachineFunction &MF) {
  if (!EnableJTHoist || skipFunction(MF.getFunction()))
    return false;

  MachineRegisterInfo &MRI = MF.getRegInfo();
  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    auto BrIt = MBB.getLastNonDebugInstr();
    if (BrIt == MBB.end() || BrIt->getOpcode() != KlaussCPU::JMPR_R)
      continue;
    MachineInstr *Load = getJumpTableLoad(*BrIt, MRI);
    if (!Load || MBB.pred_size() != 1)
      continue;
    MachineBasicBlock *Hdr = *MBB.pred_begin();
    if (Hdr == &MBB)
      continue;

    // The base and the offset, when this block computes them, each with the
    // instructions here it reads (a COPY of the index).  A root whose chain
    // has anything that cannot move stays.
    SmallVector<MachineInstr *, 4> Moves;
    for (unsigned I : {1u, 2u}) {
      SmallVector<MachineInstr *, 4> Chain;
      SmallVector<Register, 4> Work = {Load->getOperand(I).getReg()};
      bool OK = true;
      while (OK && !Work.empty()) {
        Register R = Work.pop_back_val();
        MachineInstr *Def = R.isVirtual() ? MRI.getUniqueVRegDef(R) : nullptr;
        if (!Def) {
          OK = false;
          break;
        }
        if (Def->getParent() != &MBB || is_contained(Chain, Def) ||
            is_contained(Moves, Def))
          continue;
        if (!isHoistable(*Def) || Chain.size() == 3) {
          OK = false;
          break;
        }
        Chain.push_back(Def);
        for (const MachineOperand &MO : Def->uses())
          if (MO.isReg() && MO.isUse())
            Work.push_back(MO.getReg());
      }
      if (OK)
        Moves.append(Chain);
    }
    if (Moves.empty())
      continue;
    // Keep their order: a def ahead of its uses.
    DenseMap<const MachineInstr *, unsigned> Pos;
    unsigned N = 0;
    for (const MachineInstr &MI : MBB)
      Pos[&MI] = N++;
    sort(Moves, [&](const MachineInstr *A, const MachineInstr *B) {
      return Pos[A] < Pos[B];
    });

    // In front of the header's terminators; a FLAGS clobber in front of the
    // header's last FLAGS writer, whose flags the branch (or a successor)
    // may read.
    MachineBasicBlock::iterator IP = Hdr->getFirstTerminator();
    if (any_of(Moves, [](const MachineInstr *MI) {
          return MI->definesRegister(KlaussCPU::FLAGS, /*TRI=*/nullptr);
        })) {
      auto FlagsDef = IP;
      while (FlagsDef != Hdr->begin() &&
             !std::prev(FlagsDef)->definesRegister(KlaussCPU::FLAGS,
                                                   /*TRI=*/nullptr))
        --FlagsDef;
      if (FlagsDef != Hdr->begin()) {
        // Not in front of an instruction that also reads them (add with
        // carry): the clobber would reach it.
        IP = std::prev(FlagsDef);
        if (IP->readsRegister(KlaussCPU::FLAGS, /*TRI=*/nullptr))
          continue;
      } else {
        // No FLAGS writer here: the FLAGS may come from another block.
        bool FlagsLive = any_of(Hdr->terminators(), [](const MachineInstr &T) {
          return T.readsRegister(KlaussCPU::FLAGS, /*TRI=*/nullptr);
        });
        for (const MachineBasicBlock *Succ : Hdr->successors())
          FlagsLive |= Succ->isLiveIn(KlaussCPU::FLAGS);
        if (FlagsLive)
          continue;
      }
    }

    // Each operand must already be defined at IP: by a moved instruction,
    // outside both blocks (then its def dominates the header, the dispatch
    // block's only predecessor), or in the header above IP.
    auto AvailableAtIP = [&](const MachineInstr &MI) {
      for (const MachineOperand &MO : MI.uses()) {
        if (!MO.isReg() || !MO.isUse() || !MO.getReg().isVirtual())
          continue;
        MachineInstr *Def = MRI.getUniqueVRegDef(MO.getReg());
        if (!Def)
          return false;
        if (Def->getParent() == &MBB) {
          if (!is_contained(Moves, Def))
            return false;
          continue;
        }
        if (Def->getParent() != Hdr || Def->isPHI())
          continue;
        bool Above = false;
        for (auto I = Hdr->begin(); I != IP; ++I)
          if (&*I == Def) {
            Above = true;
            break;
          }
        if (!Above)
          return false;
      }
      return true;
    };
    if (!all_of(Moves, [&](MachineInstr *MI) { return AvailableAtIP(*MI); }))
      continue;

    for (MachineInstr *MI : Moves) {
      LLVM_DEBUG(dbgs() << "JT hoist into " << printMBBReference(*Hdr) << ": "
                        << *MI);
      Hdr->splice(IP, &MBB, MI);
      // A use the moved instruction killed lives on to the header's own uses.
      for (MachineOperand &MO : MI->uses())
        if (MO.isReg() && MO.isUse() && MO.getReg().isVirtual()) {
          MO.setIsKill(false);
          MRI.clearKillFlags(MO.getReg());
        }
    }
    Changed = true;
  }
  return Changed;
}

FunctionPass *llvm::createKlaussCPUJumpTableHoistPass() {
  return new KlaussCPUJumpTableHoist();
}
