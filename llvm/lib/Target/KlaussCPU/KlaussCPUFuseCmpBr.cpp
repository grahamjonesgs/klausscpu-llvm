//===-- KlaussCPUFuseCmpBr.cpp - ISA v3 B fused compare-and-branch --------===//
//
// ISA v3 B (ISA_V3_PROPOSAL.md §5) and hw-proposals 6 (klaussCPU 3508f87,
// ISA_ENCODING_V2_MAP.md §8.1): rewrite
//
//     CMPRR  a, b     (or CMPRRW)      |   CMPRV a, #imm  (or CMPRVU, CMPRVW)
//     ...                              |   ...
//     JMPcc  target                    |   JMPcc target
//
// into one fused branch (class 0xD) at the branch's position that compares
// and branches without writing the flags: the 1-word FBR_RR (two registers)
// or FBR_RI (rhs = sext(simm4)), or the 2-word FBR_RV (rhs = sext(imm32)
// from word 1). The RTL evaluates the compare in EX on the boolean-compare
// path and redirects from MEM. PRED 0-4 compare 64 bits; PRED 5-7 (EQW,
// LTW, ULTW) the low 32 bits, as CMPRRW/CMPRVW do. There is no LEW/ULEW: a
// W compare under LE/GT/ULE/UGT swaps its registers (a <= b is !(b < a)),
// or compares with C + 1 (a <= C is a < C + 1; not for C = INT32_MAX /
// UINT32_MAX, which stay unfused). A CMPRVU constant in [2^31, 2^32) is not
// a sext(imm32), so it fuses only through a loop constant (below).
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
//  * a constant outside simm4 in a loop: when the branch is in the loop
//    header or its only latch (it runs on every iteration), and a register
//    is free in the whole loop, a SETR/SETRU in the preheader puts the
//    constant there once and the branch becomes the 1-word FBR_RR instead of
//    the 2-word FBR_RV, as does any other compare with that constant in the
//    loop. The register must be one nothing in the loop reads, writes or
//    clobbers, not live into any loop block (so not live through the loop),
//    not read by the preheader's terminators, and either caller-saved or a
//    callee-saved register the prologue already saves. -O1 and up only. A
//    second instance of this pass runs before the post-RA scheduler
//    (addPreSched2) and only does this part, turning the compare into
//    CMPRR/CMPRRW with the constant register (outer loops first), so that
//    the scheduler's anti-dependency renaming sees the register as taken.
//    It skips a function with a non-empty inline asm (see the range rule
//    below); a compare it converted that then fails the range check (below;
//    block placement runs between the two instances) stays the 1-word
//    CMPRR/CMPRRW instead of the 2-word CMPRV/CMPRVW. Any other constant
//    outside simm4 is FBR_RV;
//  * the condition maps onto a class-3 predicate: EQ/NE (incl. Z/NZ),
//    signed LT/GE/LE/GT, unsigned LT/GE/LE/GT (incl. C/NC);
//  * the flags are dead after the branch (no later reader in the block, not
//    live into any successor) — the fused form does not write them;
//  * the target is in simm13 word range (+-16 KB) of the branch, FBR_RV's
//    too (its displacement counts from its own first word), at worst case:
//    every instruction at its long (.td) size (a tail call's PseudoTAIL as
//    the 2-word TAILREL, PseudoTAILIndirect as JMPR), every aligned block
//    after its maximum padding. Compression and fusion only shrink code (the
//    fused branch replaces a 2-word JMPcc, FBR_RV included), so the PCREL13
//    fixup always resolves at assembly time (no relaxation needed). An
//    inline asm statement has no bounded size (directives, macros) unless it
//    is empty, so no branch whose range spans a non-empty one fuses.
//
// The instruction after the branch does not matter: a taken FBR squashes it
// in EX, and since klaussCPU 9ad6614 (the oldest core supported, CLAUDE.md)
// that squash also resets the divider and gates the LCD strobe, so a divide
// or an LCD write there has no effect.
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
// layout below is an upper bound already; the margin (384 bytes) is for anything
// added after this pass: KlaussCPUFetchLayout adds at most 12 four-byte NOPs per
// loop, so a branch could only fail if it spanned eight or more such loops.
constexpr int64_t MaxFusedDisp = 16000;
// Size of an instruction whose length is unknown: no branch across it fuses.
constexpr uint64_t UnboundedSize = uint64_t(1) << 20;
// Loop constants set up per function. Each SETR/SETRU (at most 8 bytes) is
// inserted after the offsets are computed, so every range check leaves room
// for all of them.
constexpr unsigned MaxLoopConsts = 16;
constexpr int64_t LoopConstBytes = 8 * MaxLoopConsts;

// The fused branch's PRED field (ISA_ENCODING_V2_MAP §8.1).
enum { EQ = 0, LT = 1, LE = 2, ULT = 3, ULE = 4, EQW = 5, LTW = 6, ULTW = 7 };

// Class-3 predicate + INV for a conditional jump that follows CMP a, b.
// Returns false for conditions with no predicate (S/NS/O/NO).
bool branchPred(unsigned Opc, unsigned &Pred, bool &Inv) {
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

bool isRegCompare(const MachineInstr &Cmp) {
  return Cmp.getOpcode() == KlaussCPU::CMPRR_I ||
         Cmp.getOpcode() == KlaussCPU::CMPRRW_I;
}

bool isWCompare(const MachineInstr &Cmp) {
  return Cmp.getOpcode() == KlaussCPU::CMPRRW_I ||
         Cmp.getOpcode() == KlaussCPU::CMPRVW_I;
}

// The W predicate for EQ, LT or ULT.
unsigned wPred(unsigned Pred) {
  assert((Pred == EQ || Pred == LT || Pred == ULT) && "no W predicate");
  return Pred == EQ ? EQW : Pred == LT ? LTW : ULTW;
}

// The fused branch for a W compare of two registers under (Pred, Inv): sets
// Swap when the registers trade places (a <= b is !(b < a), a > b is b < a).
void wRegFusion(unsigned &Pred, bool &Inv, bool &Swap) {
  Swap = Pred == LE || Pred == ULE;
  if (Swap) {
    Pred = Pred == LE ? LT : ULT;
    Inv = !Inv;
  }
  Pred = wPred(Pred);
}

// The predicate and the right-hand side (Imm, which the fused branch
// sign-extends from simm4 or imm32) with which a fused branch does what the
// immediate compare Cmp and a branch on (Pred, INV) do, or false if none
// does (INV carries over). CMPRV's imm32 is sign-extended like the fused branch's; CMPRVU's is
// zero-extended, which only [0, 2^31) also is. A W compare looks only at
// the low 32 bits of rs and of the immediate, and has no LEW/ULEW: a <= C
// becomes a < C + 1 (a > C, !(a < C + 1)), except for the largest C.
bool immFusion(const MachineInstr &Cmp, unsigned &Pred, int64_t &Imm) {
  Imm = Cmp.getOperand(1).getImm();
  if (Cmp.getOpcode() != KlaussCPU::CMPRVW_I)
    return isInt<32>(Imm);
  uint32_t C = static_cast<uint32_t>(Imm);
  if (Pred == LE || Pred == ULE) {
    if (C == (Pred == LE ? uint32_t(INT32_MAX) : UINT32_MAX))
      return false;
    ++C;
    Pred = Pred == LE ? LT : ULT;
  }
  Pred = wPred(Pred);
  Imm = static_cast<int32_t>(C);
  return true;
}

// True if the immediate compare Cmp, followed by a branch on Pred, fuses
// into the 1-word FBR_RI.
bool fitsSImm4(const MachineInstr &Cmp, unsigned Pred) {
  int64_t Imm;
  return immFusion(Cmp, Pred, Imm) && isInt<4>(Imm);
}

// The CMPRR / CMPRV / CMPRVU (or W CMPRRW / CMPRVW) whose flags the
// conditional branch Br reads, if
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
  case KlaussCPU::CMPRRW_I:
    if (!Cmp->getOperand(1).isReg())
      return nullptr;
    break;
  case KlaussCPU::CMPRV_I:
  case KlaussCPU::CMPRVU_I:
  case KlaussCPU::CMPRVW_I:
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

// A register holding the immediate of the CMPRV / CMPRVU / CMPRVW Cmp
// throughout the loop around MBB, set in the loop's preheader, or none (see
// the header). CMPRVW's (an int32) is set sign-extended, like CMPRV's: its
// W compare reads only the low 32 bits.
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
// CMPRR_I r, rC (CMPRVW_I -> CMPRRW_I), which the pre-emit instance then
// fuses like any register compare.  The post-RA scheduler renames registers to break
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
      if (Cmp && !isRegCompare(*Cmp) && !fitsSImm4(*Cmp, Pred))
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
            TII->get(isWCompare(*Cmp) ? KlaussCPU::CMPRRW_I
                                      : KlaussCPU::CMPRR_I))
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

      // The fused branch: FBR_RR for two registers (a register compare, or
      // a loop constant), else FBR_RI (simm4) or FBR_RV (imm32).
      unsigned Opc = KlaussCPU::FBR_RR;
      bool RR = isRegCompare(*Cmp);
      bool W = isWCompare(*Cmp);
      bool Swap = false;
      int64_t Imm = 0;
      Register ConstReg;
      if (!RR) {
        unsigned IPred = Pred;
        bool ImmOK = immFusion(*Cmp, IPred, Imm);
        if (ImmOK && isInt<4>(Imm)) {
          Opc = KlaussCPU::FBR_RI;
        } else if (LoopConstsOK && (ConstReg = loopConstReg(*Cmp, MBB))) {
          Opc = KlaussCPU::FBR_RR;
        } else if (ImmOK) {
          Opc = KlaussCPU::FBR_RV;
        } else {
          continue;
        }
        if (Opc != KlaussCPU::FBR_RR)
          Pred = IPred;
      }
      if (Opc == KlaussCPU::FBR_RR && W)
        wRegFusion(Pred, Inv, Swap);

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
          BuildMI(MBB, Br.getIterator(), Br.getDebugLoc(), TII->get(Opc));
      if (Opc == KlaussCPU::FBR_RR) {
        Register Lhs = Cmp->getOperand(0).getReg();
        Register Rhs = RR ? Cmp->getOperand(1).getReg() : ConstReg;
        RegState LhsFlags = RegFlags[0];
        RegState RhsFlags = RR ? RegFlags[1] : RegState{};
        if (Swap) {
          std::swap(Lhs, Rhs);
          std::swap(LhsFlags, RhsFlags);
        }
        MIB.addReg(Lhs, LhsFlags).addReg(Rhs, RhsFlags);
      } else {
        MIB.addReg(Cmp->getOperand(0).getReg(), RegFlags[0]).addImm(Imm);
      }
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
