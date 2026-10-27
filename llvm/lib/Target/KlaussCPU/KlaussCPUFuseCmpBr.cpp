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
//  * a constant outside simm4 fuses only in a loop: when the branch is in the
//    loop header or its only latch (it runs on every iteration), and a
//    register is free in the whole loop, a SETR/SETRU in the preheader puts
//    the constant there once and the branch becomes FBR_RR, as does any
//    other compare with that constant in the loop. The register
//    must be one nothing in the loop reads, writes or clobbers, not live into
//    any loop block (so not live through the loop), not read by the
//    preheader's terminators, and either caller-saved or a callee-saved
//    register the prologue already saves. -O1 and up only. A second
//    instance of this pass runs before the post-RA scheduler (addPreSched2)
//    and only does this part, turning the compare into CMPRR with the
//    constant register (outer loops first), so that the scheduler's
//    anti-dependency renaming sees the register as taken. It skips a
//    function with a non-empty inline asm (see the range rule below); a
//    compare it converted that then fails the range check stays the 1-word
//    CMPRR instead of the 2-word CMPRV;
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
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/InitializePasses.h"
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

static cl::opt<bool> EnableFuseLoopConst(
    "klausscpu-fuse-loop-const", cl::Hidden, cl::init(true),
    cl::desc("KlaussCPU: fuse a loop's compare with a constant outside simm4 "
             "by setting the constant in a free register in the preheader"));

namespace {

// simm13 words: the fused branch reaches PC-16384 .. PC+16380. The worst-case
// layout below is an upper bound already; the margin is for anything added
// after this pass.
constexpr int64_t MaxFusedDisp = 16000;
// Size of an instruction whose length is unknown: no branch across it fuses.
constexpr uint64_t UnboundedSize = uint64_t(1) << 20;
// Loop constants set up per function. Each SETR/SETRU (at most 8 bytes) is
// inserted after the offsets are computed, so every range check leaves room
// for all of them.
constexpr unsigned MaxLoopConsts = 16;
constexpr int64_t LoopConstBytes = 8 * MaxLoopConsts;

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
  /// \p LoopConstsOnly: the instance that runs before the post-RA scheduler
  /// (see the header) and only sets up the loop constants.
  explicit KlaussCPUFuseCmpBr(bool LoopConstsOnly = false)
      : MachineFunctionPass(ID), LoopConstsOnly(LoopConstsOnly) {}
  bool runOnMachineFunction(MachineFunction &MF) override;
  StringRef getPassName() const override {
    return PASS_NAME;
  }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<MachineLoopInfoWrapperPass>();
    AU.addPreserved<MachineLoopInfoWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::NoVRegs);
  }

private:
  Register loopConstReg(MachineInstr &Cmp, MachineBasicBlock &MBB);
  bool setUpLoopConsts(MachineFunction &Fn);

  bool LoopConstsOnly;

  MachineFunction *MF = nullptr;
  MachineLoopInfo *MLI = nullptr;
  const TargetInstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  // The register holding a constant in a loop: (loop, SETR/SETRU, value).
  DenseMap<std::tuple<MachineLoop *, unsigned, int64_t>, Register> LoopConsts;
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

// True if the immediate of CMPRV / CMPRVU Cmp fits FBR_RI's simm4. CMPRVU
// zero-extends its immediate: the same value as simm4 for 0..7.
bool fitsSImm4(const MachineInstr &Cmp) {
  int64_t Imm = Cmp.getOperand(1).getImm();
  int64_t Lo = Cmp.getOpcode() == KlaussCPU::CMPRVU_I ? 0 : -8;
  return Imm >= Lo && Imm <= 7;
}

// The CMPRR / CMPRV / CMPRVU whose flags the conditional branch Br reads, if
// Br can take over its compare: it is the nearest FLAGS writer before Br, and
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
  case KlaussCPU::CMPRVU_I:
    if (!Cmp->getOperand(1).isImm())
      return nullptr;
    break;
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

INITIALIZE_PASS_BEGIN(KlaussCPUFuseCmpBr, DEBUG_TYPE, PASS_NAME, false, false)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_END(KlaussCPUFuseCmpBr, DEBUG_TYPE, PASS_NAME, false, false)

// A register holding the immediate of the CMPRV / CMPRVU Cmp throughout the
// loop around MBB, set in the loop's preheader, or none (see the header).
Register KlaussCPUFuseCmpBr::loopConstReg(MachineInstr &Cmp,
                                          MachineBasicBlock &MBB) {
  MachineLoop *L = MLI->getLoopFor(&MBB);
  if (!L)
    return Register();
  unsigned SetOpc =
      Cmp.getOpcode() == KlaussCPU::CMPRVU_I ? KlaussCPU::SETRU : KlaussCPU::SETR;
  int64_t Imm = Cmp.getOperand(1).getImm();
  // Already set up for this loop or one around it: free to use anywhere in it.
  for (MachineLoop *Outer = L; Outer; Outer = Outer->getParentLoop()) {
    auto It = LoopConsts.find(std::make_tuple(Outer, SetOpc, Imm));
    if (It != LoopConsts.end())
      return It->second;
  }
  // A new one costs a SETR per entry to the loop: only for a branch that runs
  // on every iteration.
  if (&MBB != L->getHeader() && &MBB != L->getLoopLatch())
    return Register();
  MachineBasicBlock *Preheader = L->getLoopPreheader();
  if (!Preheader || LoopConsts.size() >= MaxLoopConsts)
    return Register();

  // Every register the loop touches or has live in (the loop constants set
  // up so far included), and those the preheader's terminators read.
  BitVector Used(TRI->getNumRegs());
  auto MarkUsed = [&](const MachineInstr &MI) {
    for (const MachineOperand &MO : MI.operands()) {
      if (MO.isReg() && MO.getReg().isPhysical()) {
        for (MCRegAliasIterator A(MO.getReg(), TRI, true); A.isValid(); ++A)
          Used.set(*A);
      } else if (MO.isRegMask()) {
        for (unsigned R = 1, E = TRI->getNumRegs(); R != E; ++R)
          if (MO.clobbersPhysReg(R))
            Used.set(R);
      }
    }
  };
  for (MachineBasicBlock *B : L->blocks()) {
    for (const MachineBasicBlock::RegisterMaskPair &LI : B->liveins())
      for (MCRegAliasIterator A(LI.PhysReg, TRI, true); A.isValid(); ++A)
        Used.set(*A);
    for (const MachineInstr &MI : *B)
      if (!MI.isDebugInstr())
        MarkUsed(MI);
  }
  for (const MachineInstr &MI : Preheader->terminators())
    MarkUsed(MI);

  // A callee-saved register is free to write only if the prologue saves it.
  const MachineRegisterInfo &MRI = MF->getRegInfo();
  const MachineFrameInfo &MFI = MF->getFrameInfo();
  BitVector Writable(TRI->getNumRegs(), true);
  for (const MCPhysReg *CSR = MRI.getCalleeSavedRegs(); CSR && *CSR; ++CSR)
    Writable.reset(*CSR);
  if (MFI.isCalleeSavedInfoValid())
    for (const CalleeSavedInfo &CSI : MFI.getCalleeSavedInfo())
      Writable.set(CSI.getReg());

  Register Reg;
  for (MCPhysReg R : KlaussCPU::GPRRegClass)
    if (!MRI.isReserved(R) && !Used.test(R) && Writable.test(R)) {
      Reg = R;
      break;
    }
  if (!Reg)
    return Register();

  BuildMI(*Preheader, Preheader->getFirstTerminator(), DebugLoc(),
          TII->get(SetOpc), Reg)
      .addImm(Imm);
  for (MachineBasicBlock *B : L->blocks()) {
    B->addLiveIn(Reg);
    B->sortUniqueLiveIns();
    // A stale debug location in the register would now show the constant.
    for (MachineInstr &MI : *B)
      if (MI.isDebugValue() && MI.hasDebugOperandForReg(Reg))
        MI.setDebugValueUndef();
  }
  LoopConsts[std::make_tuple(L, SetOpc, Imm)] = Reg;
  return Reg;
}

// Before the post-RA scheduler: give each fusable loop compare with a
// constant outside simm4 its loop constant now, as CMPRV_I r, imm ->
// CMPRR_I r, rC, which the pre-emit instance then fuses like any register
// compare.  The post-RA scheduler renames registers to break
// anti-dependencies and may take every free register in a loop (an unrolled
// body); with the constant already live through the loop, it leaves rC
// alone.  The range is checked when fusing: a compare left unfused is the
// 1-word CMPRR_I instead of the 2-word CMPRV_I.
bool KlaussCPUFuseCmpBr::setUpLoopConsts(MachineFunction &Fn) {
  // A non-empty inline asm blocks the branches that span it; leave such a
  // function to the pre-emit instance, which checks each branch's range.
  for (MachineBasicBlock &MBB : Fn)
    for (MachineInstr &MI : MBB)
      if (worstCaseSize(MI, *TII) >= UnboundedSize)
        return false;

  // Outer loops first, so an inner loop reuses a constant its outer loop
  // already holds (as when fusing in layout order after block placement).
  SmallVector<MachineInstr *, 8> Cmps;
  for (MachineBasicBlock &MBB : Fn) {
    for (MachineInstr &Br : MBB.terminators()) {
      unsigned Pred;
      bool Inv;
      if (!Br.isConditionalBranch() || !branchPred(Br.getOpcode(), Pred, Inv) ||
          !KlaussCPUInstrInfo::flagsDeadAfter(Br))
        continue;
      MachineInstr *Cmp = findFusableCompare(Br, TRI);
      if (Cmp && Cmp->getOpcode() != KlaussCPU::CMPRR_I && !fitsSImm4(*Cmp))
        Cmps.push_back(Cmp);
    }
  }
  llvm::stable_sort(Cmps, [&](MachineInstr *A, MachineInstr *B) {
    return MLI->getLoopDepth(A->getParent()) < MLI->getLoopDepth(B->getParent());
  });

  bool Changed = false;
  for (MachineInstr *Cmp : Cmps) {
    MachineBasicBlock &MBB = *Cmp->getParent();
    Register ConstReg = loopConstReg(*Cmp, MBB);
    if (!ConstReg)
      continue;
    const MachineOperand &Lhs = Cmp->getOperand(0);
    BuildMI(MBB, Cmp->getIterator(), Cmp->getDebugLoc(),
            TII->get(KlaussCPU::CMPRR_I))
        .addReg(Lhs.getReg(), getKillRegState(Lhs.isKill()) |
                                  getUndefRegState(Lhs.isUndef()))
        .addReg(ConstReg);
    Cmp->eraseFromParent();
    Changed = true;
  }
  return Changed;
}

bool KlaussCPUFuseCmpBr::runOnMachineFunction(MachineFunction &Fn) {
  if (!EnableFuseCmpBr)
    return false;
  // A branch into another section is resolved by the linker, and PCREL13 has
  // no relocation.
  if (Fn.hasBBSections())
    return false;
  MF = &Fn;
  MLI = &getAnalysis<MachineLoopInfoWrapperPass>().getLI();
  TII = Fn.getSubtarget().getInstrInfo();
  TRI = Fn.getSubtarget().getRegisterInfo();
  LoopConsts.clear();
  bool LoopConstsOK = EnableFuseLoopConst &&
                      Fn.getTarget().getOptLevel() != CodeGenOptLevel::None &&
                      !Fn.getFunction().hasOptNone();
  if (LoopConstsOnly)
    return LoopConstsOK && setUpLoopConsts(Fn);

  // Worst-case offset of every block: instructions at their long encodings,
  // each aligned block after its largest padding (code is word aligned, so
  // at most Align - 4 bytes). The real distance between two points is never
  // larger than the distance between their offsets here, plus the loop
  // constants inserted below.
  SmallVector<uint64_t, 16> BlockOffset(Fn.getNumBlockIDs());
  uint64_t Offset = 0;
  for (MachineBasicBlock &MBB : Fn) {
    uint64_t A = MBB.getAlignment().value();
    if (A > 4)
      Offset += A - 4;
    BlockOffset[MBB.getNumber()] = Offset;
    for (MachineInstr &MI : MBB)
      Offset += worstCaseSize(MI, *TII);
  }
  const int64_t Limit = MaxFusedDisp - (LoopConstsOK ? LoopConstBytes : 0);

  bool Changed = false;
  for (MachineBasicBlock &MBB : Fn) {
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
      if (Disp < -Limit || Disp > Limit)
        continue;
      if (!KlaussCPUInstrInfo::flagsDeadAfter(Br))
        continue;
      MachineInstr *Cmp = findFusableCompare(Br, TRI);
      if (!Cmp)
        continue;
      bool RR = Cmp->getOpcode() == KlaussCPU::CMPRR_I;
      Register ConstReg;
      if (!RR && !fitsSImm4(*Cmp)) {
        if (!LoopConstsOK || !(ConstReg = loopConstReg(*Cmp, MBB)))
          continue;
      }

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
      MachineInstrBuilder MIB =
          BuildMI(MBB, Br.getIterator(), Br.getDebugLoc(),
                  TII->get(RR || ConstReg ? KlaussCPU::FBR_RR
                                          : KlaussCPU::FBR_RI))
              .addReg(Cmp->getOperand(0).getReg(), RegFlags[0]);
      if (RR)
        MIB.addReg(Cmp->getOperand(1).getReg(), RegFlags[1]);
      else if (ConstReg)
        MIB.addReg(ConstReg);
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
FunctionPass *createKlaussCPUFuseCmpBrPass(bool LoopConstsOnly) {
  return new KlaussCPUFuseCmpBr(LoopConstsOnly);
}
} // namespace llvm
