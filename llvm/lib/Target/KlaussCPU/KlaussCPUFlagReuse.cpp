//===-- KlaussCPUFlagReuse.cpp - Reuse the arithmetic Z flag --------------===//
//
// KlaussCPU LLVM backend — A3a: arithmetic Z-flag reuse (post-RA peephole).
//
// A `(X == 0)` / `(X != 0)` branch whose X is produced by a flag-setting
// op (arithmetic, a shift, a multiply, ...; setsZeroFlagAsResult) does not
// need a compare: that op already set the hardware Z flag to `(X == 0)`.
// This pass rewrites the already-scheduled sequence
//
//     <arith> Rd, ...        ; sets Z = (Rd == 0)  (and S/C/V)
//     cmprv   Rd, 0          ; CMP is now SUB-without-writeback → re-derives
//                            ;   the identical Z            ← redundant
//     jmpe/jmpne  <target>   ; equality reads Z (JMPE ≡ JMPZ post-unification)
//
// into
//
//     <arith> Rd, ...
//     jmpz/jmpnz  <target>   ; reads Z
//
// deleting the CMPRV and its execute cycles from every matching loop
// iteration.  Instructions that write no flag may sit between the three.
// On a fetch-bound core (fetch = 59-75% of cycles) that directly cuts
// fetched words per iteration.
//
// Why a post-RA MI peephole and not a DAG-level rewrite: dropping the compare
// at selection time requires gluing the arithmetic op adjacent to the branch,
// which is unschedulable whenever the arithmetic result also feeds a loop
// back-edge (its CopyToReg has nowhere to land) — it deadlocks the DAG
// scheduler.  Operating on the final, already-scheduled instruction stream
// avoids that entirely; the modelled FLAGS defs show that no flag-clobbering
// instruction sits between the producer and the branch.
//
// Flag model (post-unification — see FLAG_UNIFICATION_CHANGES / RTL fbb77d7):
// there is ONE flags register Z/S/C/V.  Equality is Z for BOTH arithmetic ops
// and CMP (CMP is now SUB without writeback), so JMPE and JMPZ read the very
// same bit.  This pass canonicalises the branch to JMPZ/JMPNZ and drops the
// compare; it fires only when the producer is an op whose Z is exactly
// `(result == 0)`.  (Before unification JMPE read a *separate*
// equal_flag that arithmetic did not set — which is why this was gated OFF;
// that hazard no longer exists.)  Only Z matches: the arithmetic op's C and V
// are not those of `CMP Rd, 0`, so the pass also requires that no instruction
// after the branch, here or in a successor, reads the compare's flags.
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
#define PASS_NAME "KlaussCPU arithmetic Z-flag reuse"

namespace llvm {
void initializeKlaussCPUFlagReusePass(PassRegistry &);
} // namespace llvm

// Default ON since the flag unification (RTL fbb77d7 / FLAG_UNIFICATION_CHANGES):
// equality is now the single Z flag for both arithmetic ops and CMP, so dropping
// the redundant `CMPRV Rd,0` and reading Z is safe whenever the equality branch
// is the only reader of the compare's flags — the old "which hardware flag does
// the branch read" hazard is gone.  Kept as an escape hatch
// (`-klausscpu-arith-flag-reuse=false`) for A/B measurement and bisection.
static cl::opt<bool> EnableArithFlagReuse(
    "klausscpu-arith-flag-reuse", cl::Hidden, cl::init(true),
    cl::desc("KlaussCPU: reuse the arithmetic Z flag for (X==0)/(X!=0) "
             "branches, deleting the redundant CMPRV before the equality jump"));

namespace {

// An op that sets Z = (result == 0), with the result in operand 0.  The CPU
// sets Z from the value it writes back (pipeline_core.sv: wb_zval, Z =
// (wb_value == 0), for every flag writer but BTST) whenever the op's flag
// mask has Z: ADD/SUB and their W and immediate forms, INC/DEC, the shifts
// and rotates (F = 1 in every template), NEG/NOT/ABS/SEXTB/SEXTH/ZEXTB/
// ZEXTH/POPCNT (F = 1), BEXTR and MUL.  Not DIV/MOD: a zero divisor writes
// only V.  The runtime's test_flags checks Z == (rd == 0) for each of these
// on the emulator or the board (gen_test_flags.py reads this list).
static bool setsZeroFlagAsResult(unsigned Opc) {
  switch (Opc) {
  case KlaussCPU::INCR:
  case KlaussCPU::DECR:
  case KlaussCPU::ADDR:
  case KlaussCPU::SUBR:
  case KlaussCPU::ADDW:
  case KlaussCPU::SUBW:
  case KlaussCPU::ADDI:
  case KlaussCPU::ADDIW:
  case KlaussCPU::ADDV:
  case KlaussCPU::MINUSV:
  case KlaussCPU::NEGR:
  case KlaussCPU::NOTR:
  case KlaussCPU::ABSR:
  case KlaussCPU::SEXTB:
  case KlaussCPU::SEXTH:
  case KlaussCPU::ZEXTB:
  case KlaussCPU::ZEXTH:
  case KlaussCPU::POPCNT:
  case KlaussCPU::SHLR:
  case KlaussCPU::SHRR:
  case KlaussCPU::SARR:
  case KlaussCPU::ROLR:
  case KlaussCPU::RORR_R:
  case KlaussCPU::SHLV:
  case KlaussCPU::SHRV:
  case KlaussCPU::SHRAV:
  case KlaussCPU::ROLV:
  case KlaussCPU::RORV:
  case KlaussCPU::BEXTR:
  case KlaussCPU::MULR:
  case KlaussCPU::MULUR:
  case KlaussCPU::MULHR:
  case KlaussCPU::MULHUR:
  case KlaussCPU::MULV:
  case KlaussCPU::MULW:
    return true;
  default:
    return false;
  }
}

// Map an equality branch (JMPE/JMPNE) to its canonical zero-flag form. Both
// read Z post-unification; this normalises the mnemonic after dropping the CMP.
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
    return PASS_NAME;
  }

  MachineFunctionProperties getRequiredProperties() const override {
    // Runs post-RA (addPreEmitPass): operands are physical registers.
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::NoVRegs);
  }
};

} // end anonymous namespace

char KlaussCPUFlagReuse::ID = 0;

INITIALIZE_PASS(KlaussCPUFlagReuse, DEBUG_TYPE, PASS_NAME, false, false)

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

      // Find the most recent FLAGS writer before the compare.  It must be a
      // Z-setting arithmetic op writing exactly Rd, and nothing in between may
      // redefine Rd.  Instructions in between are known not to touch FLAGS:
      // every flag-writing instruction declares Defs=[FLAGS] (the same
      // invariant the schedulers rely on).  Calls and inline asm are treated
      // as barriers, since what they do to the flags is not modelled.
      MachineInstr *Prev = nullptr;
      for (MachineInstr *I = MI.getPrevNode(); I; I = I->getPrevNode()) {
        if (I->isDebugInstr())
          continue;
        if (I->isCall() || I->isInlineAsm())
          break;
        if (I->modifiesRegister(KlaussCPU::FLAGS, /*TRI=*/nullptr)) {
          Prev = I;
          break;
        }
        if (I->modifiesRegister(Rd, /*TRI=*/nullptr))
          break;
      }
      if (!Prev || !setsZeroFlagAsResult(Prev->getOpcode()))
        continue;
      if (Prev->getNumOperands() < 1 || !Prev->getOperand(0).isReg() ||
          Prev->getOperand(0).getReg() != Rd)
        continue;

      // The next instruction must be the conditional branch that consumes
      // this compare's Z flag (JMPE/JMPNE — equality).  Debug instructions
      // in between are skipped (they never read FLAGS), so -g does not
      // change codegen.
      auto BrIt = next_nodbg(MachineBasicBlock::iterator(MI), MBB.end());
      if (BrIt == MBB.end())
        continue;
      MachineInstr *Br = &*BrIt;
      unsigned NewBrOpc = zeroFlagBranchOpc(Br->getOpcode());
      if (!NewBrOpc)
        continue;

      // The branch must be the compare's only flag consumer.  It is not when
      // MachineCSE dropped an identical compare from a successor, leaving
      // e.g. a JMPLT there that reads these flags: the arithmetic op's C and
      // V differ from CMP's (CMP Rd,0 always clears V; ADDR sets it on
      // signed overflow), so only Z may be taken from the arithmetic op.
      if (!KlaussCPUInstrInfo::flagsDeadAfter(*Br))
        continue;

      // The producer's FLAGS def is now what the branch reads.  It was
      // emitted `dead` (nothing read it while the compare sat in between);
      // leaving that marking would make the branch read an undefined physreg.
      MachineOperand *FlagDef =
          Prev->findRegisterDefOperand(KlaussCPU::FLAGS, /*TRI=*/nullptr);
      if (!FlagDef)
        continue;
      FlagDef->setIsDead(false);

      // Rewrite the branch to read Z and drop the redundant compare.
      Br->setDesc(TII->get(NewBrOpc));
      DeadCompares.push_back(&MI);
      Changed = true;
      LLVM_DEBUG(dbgs() << "KlaussCPUFlagReuse: dropped CMPRV, "
                        << "branch now reads Z in " << MBB.getName()
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
