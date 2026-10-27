//===-- KlaussCPUFuseCmpBr.cpp - ISA v3 B fused compare-and-branch --------===//
//
// ISA v3 B (ISA_V3_PROPOSAL.md §5): rewrite
//
//     CMPRR  a, b            |   CMPRV a, #imm   (imm in -8..7;
//     ...                    |   ...              CMPRVU: 0..7)
//     JMPcc  target          |   JMPcc target
//
// into one 1-word fused branch (class 0xD, FBR_RR / FBR_RI) at the branch's
// position that compares and branches without writing the flags. The RTL
// evaluates the compare in EX on the boolean-compare path and redirects from
// MEM. FBR has no width bit: it always compares 64 bits, so the W compares
// (CMPRRW/CMPRVW) never fuse.
//
// Conditions, all checked here on the final (post-RA, pre-emit) stream:
//  * the compare is the nearest FLAGS writer before the branch in its block,
//    and every instruction between them (the PHI copies, constant and address
//    materialisations and spills that PHI elimination and the schedulers put
//    there) reads no FLAGS, redefines no compare operand, and is not a call,
//    inline asm or other unmodelled side effect. The compare then computes
//    the same thing at the branch, and the instructions in between never
//    observed its flags. Debug instructions are ignored (-g does not change
//    codegen) and stay where they are;
//  * the condition maps onto a class-3 predicate: EQ/NE (incl. Z/NZ),
//    signed LT/GE/LE/GT, unsigned LT/GE/LE/GT (incl. C/NC);
//  * the flags are dead after the branch (no later reader in the block, not
//    live into any successor) — the fused form does not write them;
//  * the target is in simm13 word range (+-16 KB) at worst case: every
//    instruction at its long (.td) size, every aligned block after its
//    maximum padding. Compression and fusion only shrink code, so the PCREL13
//    fixup always resolves at assembly time (no relaxation needed). An inline
//    asm statement has no bounded size (directives, macros) unless it is
//    empty, so no branch whose range spans a non-empty one fuses.
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

// The pass name must differ from the -klausscpu-fuse-cmp-br option below: opt
// registers every legacy pass name as a command-line option, and a clash
// aborts opt at startup ("registered more than once").
#define DEBUG_TYPE "klausscpu-cmp-br-fuse"
#define PASS_NAME "KlaussCPU fused compare-and-branch (ISA v3 B)"

namespace llvm {
void initializeKlaussCPUFuseCmpBrPass(PassRegistry &);
} // namespace llvm

using namespace llvm;

static cl::opt<bool> EnableFuseCmpBr(
    "klausscpu-fuse-cmp-br", cl::Hidden, cl::init(true),
    cl::desc("KlaussCPU ISA v3 B: fuse compare + conditional branch"));

namespace {

// simm13 words: the fused branch reaches PC-16384 .. PC+16380. The worst-case
// layout below is an upper bound already; the margin is for anything added
// after this pass.
constexpr int64_t MaxFusedDisp = 16000;
// Size of an instruction whose length is unknown: no branch across it fuses.
constexpr uint64_t UnboundedSize = uint64_t(1) << 20;

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

// Worst-case size in bytes of MI. An inline asm statement can be a directive
// (.space, .rept) or a macro of any length, so only an empty one is bounded.
uint64_t worstCaseSize(const MachineInstr &MI, const TargetInstrInfo &TII) {
  if (MI.isInlineAsm()) {
    StringRef Asm = MI.getOperand(0).getSymbolName();
    return Asm.trim().empty() ? 0 : UnboundedSize;
  }
  return TII.get(MI.getOpcode()).getSize();
}

// The CMPRR / CMPRV whose flags the conditional branch Br reads, if Br can
// take over its compare: it is the nearest FLAGS writer before Br, and
// nothing between them reads FLAGS, redefines a compare operand, or has an
// effect on FLAGS that is not modelled (calls, inline asm, side effects).
MachineInstr *findFusableCompare(MachineInstr &Br,
                                 const TargetRegisterInfo *TRI) {
  MachineBasicBlock &MBB = *Br.getParent();
  MachineInstr *Cmp = nullptr;
  for (MachineInstr &MI :
       make_range(std::next(MachineBasicBlock::reverse_iterator(Br)),
                  MBB.rend())) {
    if (MI.isDebugInstr())
      continue;
    if (MI.isCall() || MI.isInlineAsm() || MI.hasUnmodeledSideEffects() ||
        MI.readsRegister(KlaussCPU::FLAGS, TRI))
      return nullptr;
    if (MI.modifiesRegister(KlaussCPU::FLAGS, TRI)) {
      Cmp = &MI;
      break;
    }
  }
  if (!Cmp)
    return nullptr;

  switch (Cmp->getOpcode()) {
  case KlaussCPU::CMPRR_I:
    if (!Cmp->getOperand(1).isReg())
      return nullptr;
    break;
  case KlaussCPU::CMPRV_I:
  case KlaussCPU::CMPRVU_I: {
    // CMPRVU zero-extends its immediate: the same value as simm4 for 0..7.
    if (!Cmp->getOperand(1).isImm())
      return nullptr;
    int64_t Imm = Cmp->getOperand(1).getImm();
    int64_t Lo = Cmp->getOpcode() == KlaussCPU::CMPRVU_I ? 0 : -8;
    if (Imm < Lo || Imm > 7)
      return nullptr;
    break;
  }
  default:
    return nullptr;
  }
  if (!Cmp->getOperand(0).isReg())
    return nullptr;

  // Br will read the compare operands where it stands.
  for (MachineInstr &MI :
       make_range(std::next(Cmp->getIterator()), Br.getIterator()))
    for (unsigned I = 0; I < 2; ++I) {
      const MachineOperand &MO = Cmp->getOperand(I);
      if (MO.isReg() && MI.modifiesRegister(MO.getReg(), TRI))
        return nullptr;
    }
  return Cmp;
}

} // namespace

char KlaussCPUFuseCmpBr::ID = 0;

INITIALIZE_PASS(KlaussCPUFuseCmpBr, DEBUG_TYPE, PASS_NAME, false, false)

bool KlaussCPUFuseCmpBr::runOnMachineFunction(MachineFunction &MF) {
  if (!EnableFuseCmpBr)
    return false;
  // A branch into another section is resolved by the linker, and PCREL13 has
  // no relocation.
  if (MF.hasBBSections())
    return false;
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();

  // Worst-case offset of every block: instructions at their long encodings,
  // each aligned block after its largest padding (code is word aligned, so
  // at most Align - 4 bytes). The real distance between two points is never
  // larger than the distance between their offsets here.
  SmallVector<uint64_t, 16> BlockOffset(MF.getNumBlockIDs());
  uint64_t Offset = 0;
  for (MachineBasicBlock &MBB : MF) {
    uint64_t A = MBB.getAlignment().value();
    if (A > 4)
      Offset += A - 4;
    BlockOffset[MBB.getNumber()] = Offset;
    for (MachineInstr &MI : MBB)
      Offset += worstCaseSize(MI, *TII);
  }

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    SmallVector<MachineInstr *, 4> Dead;
    uint64_t BrOffset = BlockOffset[MBB.getNumber()];
    for (MachineInstr &Br : MBB) {
      uint64_t ThisOffset = BrOffset;
      BrOffset += worstCaseSize(Br, *TII);
      if (!Br.isConditionalBranch() || Br.getNumOperands() < 1 ||
          !Br.getOperand(0).isMBB())
        continue;
      unsigned Pred;
      bool Inv;
      if (!branchPred(Br.getOpcode(), Pred, Inv))
        continue;
      MachineBasicBlock *Target = Br.getOperand(0).getMBB();
      int64_t Disp = int64_t(BlockOffset[Target->getNumber()]) -
                     int64_t(ThisOffset);
      if (Disp < -MaxFusedDisp || Disp > MaxFusedDisp)
        continue;
      if (!KlaussCPUInstrInfo::flagsDeadAfter(Br))
        continue;
      MachineInstr *Cmp = findFusableCompare(Br, TRI);
      if (!Cmp)
        continue;

      // The compare operands are now read at Br: move their kill flags (from
      // the compare, or from a last use in between) onto the fused branch.
      RegState RegFlags[2] = {};
      for (unsigned I = 0; I < 2; ++I) {
        const MachineOperand &MO = Cmp->getOperand(I);
        if (!MO.isReg())
          continue;
        bool Kill = MO.isKill();
        for (MachineInstr &MI :
             make_range(std::next(Cmp->getIterator()), Br.getIterator()))
          if (MI.killsRegister(MO.getReg(), TRI)) {
            MI.clearRegisterKills(MO.getReg(), TRI);
            Kill = true;
          }
        RegFlags[I] = getKillRegState(Kill) | getUndefRegState(MO.isUndef());
      }

      int64_t CC = Pred | (Inv ? 8 : 0);
      bool RR = Cmp->getOpcode() == KlaussCPU::CMPRR_I;
      MachineInstrBuilder MIB =
          BuildMI(MBB, Br.getIterator(), Br.getDebugLoc(),
                  TII->get(RR ? KlaussCPU::FBR_RR : KlaussCPU::FBR_RI))
              .addReg(Cmp->getOperand(0).getReg(), RegFlags[0]);
      if (RR)
        MIB.addReg(Cmp->getOperand(1).getReg(), RegFlags[1]);
      else
        MIB.addImm(Cmp->getOperand(1).getImm());
      MIB.addImm(CC).addMBB(Target);
      Dead.push_back(Cmp);
      Dead.push_back(&Br);
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
