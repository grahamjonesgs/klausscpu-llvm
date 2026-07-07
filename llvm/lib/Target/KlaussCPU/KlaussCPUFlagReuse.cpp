//===-- KlaussCPUFlagReuse.cpp - Reuse arithmetic zero_flag ---------------===//
//
// KlaussCPU LLVM backend — A3a: arithmetic zero_flag reuse (post-RA peephole).
//
// A `(X == 0)` / `(X != 0)` branch whose X is produced by a flag-setting
// arithmetic op does not need a compare: that op already set the hardware
// zero_flag to `(X == 0)`.  This pass rewrites the already-scheduled triple
//
//     <arith> Rd, ...        ; sets zero_flag = (Rd == 0)
//     cmprv   Rd, 0          ; sets equal_flag = (Rd == 0)   ← redundant
//     jmpe/jmpne  <target>   ; reads equal_flag
//
// into
//
//     <arith> Rd, ...
//     jmpz/jmpnz  <target>   ; reads zero_flag
//
// deleting the 8-byte CMPRV and its execute cycles from every matching loop
// iteration.  On a fetch-bound core (fetch = 59-75% of cycles) that directly
// cuts fetched words per iteration.
//
// Why a post-RA MI peephole and not a DAG-level rewrite: dropping the compare
// at selection time requires gluing the arithmetic op adjacent to the branch,
// which is unschedulable whenever the arithmetic result also feeds a loop
// back-edge (its CopyToReg has nowhere to land) — it deadlocks the DAG
// scheduler.  Operating on the final, already-scheduled instruction stream
// avoids that entirely, and adjacency in that stream is itself the proof that
// no flag-clobbering instruction sits between the producer and the branch.
//
// Flag discipline (CPU_ARCHITECTURE.md §6 — a wrong-flag branch silently
// miscompiles): zero_flag is set by ARITHMETIC ops and read by JMPZ/JMPNZ;
// equal_flag is a DIFFERENT register set only by CMPRR/CMPRV and read by
// JMPE/JMPNE.  This pass only ever routes to JMPZ/JMPNZ, and only when the
// producer is an arithmetic op whose zero_flag is exactly `(result == 0)`.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUInstrInfo.h"
#include "KlaussCPUSubtarget.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "klausscpu-flag-reuse"

// Default OFF: this changes which hardware flag a branch reads, so it must be
// validated by the on-silicon regression suite (queens/test_64bit/bst plus the
// branchy/calls_fib perf kernels) before being promoted to the default.  Flip
// to cl::init(true) once that board run is green.
static cl::opt<bool> EnableArithFlagReuse(
    "klausscpu-arith-flag-reuse", cl::Hidden, cl::init(false),
    cl::desc("KlaussCPU: reuse the arithmetic zero_flag for (X==0)/(X!=0) "
             "branches, deleting the redundant CMPRV before JMPE/JMPNE"));

namespace {

// An arithmetic op sets zero_flag = (result == 0).  Only opcodes whose result
// register is exactly the value being tested for zero qualify.  Kept
// deliberately narrow: every entry must be an op that (a) writes its result to
// operand 0 and (b) is documented to set zero_flag on that result.
static bool setsZeroFlagAsResult(unsigned Opc) {
  switch (Opc) {
  case KlaussCPU::INCR:   // rd = rd + 1
  case KlaussCPU::DECR:   // rd = rd - 1
  case KlaussCPU::ADDR:   // rd = rs1 + rs2
  case KlaussCPU::SUBR:   // rd = rs1 - rs2
    return true;
  default:
    return false;
  }
}

// Map an equal_flag branch to the corresponding zero_flag branch.
static unsigned zeroFlagBranchOpc(unsigned Opc) {
  switch (Opc) {
  case KlaussCPU::JMPE:      return KlaussCPU::JMPZ;
  case KlaussCPU::JMPNE:     return KlaussCPU::JMPNZ;
  case KlaussCPU::JMPEREL:   return KlaussCPU::JMPZREL;
  case KlaussCPU::JMPNEREL:  return KlaussCPU::JMPNZREL;
  default:                   return 0;
  }
}

class KlaussCPUFlagReuse : public MachineFunctionPass {
public:
  static char ID;
  KlaussCPUFlagReuse() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override {
    return "KlaussCPU arithmetic zero_flag reuse";
  }

  MachineFunctionProperties getRequiredProperties() const override {
    // Runs post-RA (addPreEmitPass): operands are physical registers.
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::NoVRegs);
  }
};

} // end anonymous namespace

char KlaussCPUFlagReuse::ID = 0;

bool KlaussCPUFlagReuse::runOnMachineFunction(MachineFunction &MF) {
  if (!EnableArithFlagReuse)
    return false;

  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  bool Changed = false;

  for (MachineBasicBlock &MBB : MF) {
    SmallVector<MachineInstr *, 4> DeadCompares;
    for (MachineInstr &MI : MBB) {
      // Look for `cmprv Rd, 0` (operand 0 = compared reg, operand 1 = imm).
      if (MI.getOpcode() != KlaussCPU::CMPRV_I)
        continue;
      if (MI.getNumOperands() < 2 || !MI.getOperand(0).isReg() ||
          !MI.getOperand(1).isImm() || MI.getOperand(1).getImm() != 0)
        continue;
      Register Rd = MI.getOperand(0).getReg();

      // The instruction immediately before must be a zero_flag arithmetic op
      // writing exactly Rd.  Adjacency proves nothing clobbers zero_flag
      // between the producer and (after deletion) the branch.
      MachineInstr *Prev = MI.getPrevNode();
      if (!Prev || Prev->isMetaInstruction())
        continue;
      if (!setsZeroFlagAsResult(Prev->getOpcode()))
        continue;
      if (Prev->getNumOperands() < 1 || !Prev->getOperand(0).isReg() ||
          Prev->getOperand(0).getReg() != Rd)
        continue;

      // The instruction immediately after must be the conditional branch that
      // consumes this compare's equal_flag (JMPE/JMPNE).  It is the compare's
      // only flag consumer: each BR_CC emits its own compare, and the branch
      // is a terminator so control leaves the block after it.
      MachineInstr *Br = MI.getNextNode();
      if (!Br || Br->isMetaInstruction())
        continue;
      unsigned NewBrOpc = zeroFlagBranchOpc(Br->getOpcode());
      if (!NewBrOpc)
        continue;

      // Rewrite the branch to read zero_flag and drop the redundant compare.
      Br->setDesc(TII->get(NewBrOpc));
      DeadCompares.push_back(&MI);
      Changed = true;
      LLVM_DEBUG(dbgs() << "KlaussCPUFlagReuse: dropped CMPRV, "
                        << "branch now reads zero_flag in " << MBB.getName()
                        << "\n");
    }
    for (MachineInstr *MI : DeadCompares)
      MI->eraseFromParent();
  }

  return Changed;
}

namespace llvm {
FunctionPass *createKlaussCPUFlagReusePass() { return new KlaussCPUFlagReuse(); }
} // namespace llvm
