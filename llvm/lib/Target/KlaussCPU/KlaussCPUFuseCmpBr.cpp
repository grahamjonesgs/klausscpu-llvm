//===-- KlaussCPUFuseCmpBr.cpp - ISA v3 B fused compare-and-branch --------===//
//
// ISA v3 B (ISA_V3_PROPOSAL.md §5): rewrite an adjacent
//
//     CMPRR  a, b            |   CMPRV a, #imm   (imm in -8..7)
//     JMPcc  target          |   JMPcc target
//
// into one 1-word fused branch (class 0xD, FBR_RR / FBR_RI) that compares
// and branches without writing the flags. The RTL evaluates the compare in
// EX on the boolean-compare path and redirects from MEM.
//
// Conditions, all checked here on the final (post-RA, pre-emit) stream:
//  * the compare and the branch are adjacent (debug instructions in between
//    are ignored, so -g does not change codegen; they stay where they are);
//  * the condition maps onto a class-3 predicate: EQ/NE (incl. Z/NZ),
//    signed LT/GE/LE/GT, unsigned LT/GE/LE/GT (incl. C/NC);
//  * the flags are dead after the branch (no later reader in the block, not
//    live into any successor) — the fused form does not write them;
//  * the whole function is under 16 KB at worst-case (2/3-word) sizes, so
//    every intra-function displacement fits the simm13 word field and the
//    PCREL13 fixup always resolves at assembly time (no relaxation needed).
//
// -mllvm -klausscpu-fuse-cmp-br=false disables it.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUInstrInfo.h"
#include "KlaussCPUSubtarget.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Support/CommandLine.h"

#define DEBUG_TYPE "klausscpu-fuse-cmp-br"
#define PASS_NAME "KlaussCPU fused compare-and-branch (ISA v3 B)"

namespace llvm {
void initializeKlaussCPUFuseCmpBrPass(PassRegistry &);
} // namespace llvm

using namespace llvm;

static cl::opt<bool> EnableFuseCmpBr(
    "klausscpu-fuse-cmp-br", cl::Hidden, cl::init(true),
    cl::desc("KlaussCPU ISA v3 B: fuse compare + conditional branch"));

namespace {

// Class-3 predicate + INV for a conditional jump that follows CMP a, b.
// Returns false for conditions with no predicate (S/NS/O/NO).
bool branchPred(unsigned Opc, unsigned &Pred, bool &Inv) {
  enum { EQ = 0, LT = 1, LE = 2, ULT = 3, ULE = 4 };
  switch (Opc) {
  case KlaussCPU::JMPE:   case KlaussCPU::JMPEREL:
  case KlaussCPU::JMPZ:   case KlaussCPU::JMPZREL:   Pred = EQ;  Inv = false; return true;
  case KlaussCPU::JMPNE:  case KlaussCPU::JMPNEREL:
  case KlaussCPU::JMPNZ:  case KlaussCPU::JMPNZREL:  Pred = EQ;  Inv = true;  return true;
  case KlaussCPU::JMPLT:  case KlaussCPU::JMPLTREL:  Pred = LT;  Inv = false; return true;
  case KlaussCPU::JMPGE:  case KlaussCPU::JMPGEREL:  Pred = LT;  Inv = true;  return true;
  case KlaussCPU::JMPLE:  case KlaussCPU::JMPLEREL:  Pred = LE;  Inv = false; return true;
  case KlaussCPU::JMPGT:  case KlaussCPU::JMPGTREL:  Pred = LE;  Inv = true;  return true;
  case KlaussCPU::JMPULT: case KlaussCPU::JMPULTREL:
  case KlaussCPU::JMPC:   case KlaussCPU::JMPCREL:   Pred = ULT; Inv = false; return true;
  case KlaussCPU::JMPUGE: case KlaussCPU::JMPUGEREL:
  case KlaussCPU::JMPNC:  case KlaussCPU::JMPNCREL:  Pred = ULT; Inv = true;  return true;
  case KlaussCPU::JMPULE: case KlaussCPU::JMPULEREL: Pred = ULE; Inv = false; return true;
  case KlaussCPU::JMPUGT: case KlaussCPU::JMPUGTREL: Pred = ULE; Inv = true;  return true;
  default: return false;
  }
}

class KlaussCPUFuseCmpBr : public MachineFunctionPass {
public:
  static char ID;
  KlaussCPUFuseCmpBr() : MachineFunctionPass(ID) {}
  bool runOnMachineFunction(MachineFunction &MF) override;
  StringRef getPassName() const override {
    return PASS_NAME;
  }
  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::NoVRegs);
  }
};

} // namespace

char KlaussCPUFuseCmpBr::ID = 0;

INITIALIZE_PASS(KlaussCPUFuseCmpBr, DEBUG_TYPE, PASS_NAME, false, false)

bool KlaussCPUFuseCmpBr::runOnMachineFunction(MachineFunction &MF) {
  if (!EnableFuseCmpBr)
    return false;
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();

  // Worst-case function size (every instruction at its long encoding).
  uint64_t Size = 0;
  for (MachineBasicBlock &MBB : MF)
    for (MachineInstr &MI : MBB) {
      if (MI.isInlineAsm())
        return false; // unknown length: don't risk an out-of-range fixup
      Size += TII->get(MI.getOpcode()).getSize();
    }
  if (Size >= 16000) // simm13 words = +-16 KB
    return false;

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    SmallVector<MachineInstr *, 4> Dead;
    for (MachineInstr &Cmp : MBB) {
      unsigned CO = Cmp.getOpcode();
      if (CO != KlaussCPU::CMPRR_I && CO != KlaussCPU::CMPRV_I)
        continue;
      // A debug instruction never reads FLAGS or changes a register, so it
      // does not separate the pair.
      auto BrIt = next_nodbg(MachineBasicBlock::iterator(Cmp), MBB.end());
      if (BrIt == MBB.end())
        continue;
      MachineInstr *Br = &*BrIt;
      if (!Br->isConditionalBranch() || Br->getNumOperands() < 1 ||
          !Br->getOperand(0).isMBB())
        continue;
      unsigned Pred;
      bool Inv;
      if (!branchPred(Br->getOpcode(), Pred, Inv))
        continue;
      if (CO == KlaussCPU::CMPRV_I &&
          (!Cmp.getOperand(1).isImm() || Cmp.getOperand(1).getImm() < -8 ||
           Cmp.getOperand(1).getImm() > 7))
        continue;
      if (!KlaussCPUInstrInfo::flagsDeadAfter(*Br))
        continue;

      int64_t CC = Pred | (Inv ? 8 : 0);
      MachineInstrBuilder MIB =
          BuildMI(MBB, Br->getIterator(), Br->getDebugLoc(),
                  TII->get(CO == KlaussCPU::CMPRR_I ? KlaussCPU::FBR_RR
                                                    : KlaussCPU::FBR_RI))
              .addReg(Cmp.getOperand(0).getReg());
      if (CO == KlaussCPU::CMPRR_I)
        MIB.addReg(Cmp.getOperand(1).getReg());
      else
        MIB.addImm(Cmp.getOperand(1).getImm());
      MIB.addImm(CC).addMBB(Br->getOperand(0).getMBB());
      Dead.push_back(&Cmp);
      Dead.push_back(Br);
      Changed = true;
    }
    for (MachineInstr *MI : Dead)
      MI->eraseFromParent();
  }
  return Changed;
}

namespace llvm {
FunctionPass *createKlaussCPUFuseCmpBrPass() { return new KlaussCPUFuseCmpBr(); }
} // namespace llvm
