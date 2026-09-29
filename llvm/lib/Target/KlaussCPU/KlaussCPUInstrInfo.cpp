//===-- KlaussCPUInstrInfo.cpp - KlaussCPU Instruction Information --------===//
//
// KlaussCPU LLVM backend — instruction info implementation.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUInstrInfo.h"
#include "KlaussCPURegisterInfo.h"
#include "KlaussCPUSubtarget.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

#define GET_INSTRINFO_CTOR_DTOR
#include "KlaussCPUGenInstrInfo.inc"

using namespace llvm;

KlaussCPUInstrInfo::KlaussCPUInstrInfo(const KlaussCPUSubtarget &STI_,
                                        const KlaussCPURegisterInfo &RI)
    : KlaussCPUGenInstrInfo(STI_, RI,
                            KlaussCPU::ADJCALLSTACKDOWN,
                            KlaussCPU::ADJCALLSTACKUP),
      STI(STI_) {}

void KlaussCPUInstrInfo::storeRegToStackSlot(MachineBasicBlock &MBB,
                                              MachineBasicBlock::iterator I,
                                              Register SrcReg, bool IsKill,
                                              int FI,
                                              const TargetRegisterClass *RC,
                                              Register VReg,
                                              MachineInstr::MIFlag Flags) const {
  assert(KlaussCPU::GPRRegClass.hasSubClassEq(RC) &&
         "KlaussCPU storeRegToStackSlot: only GPR spills supported");
  MachineFunction &MF = *MBB.getParent();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  MachineMemOperand *MMO = MF.getMachineMemOperand(
      MachinePointerInfo::getFixedStack(MF, FI), MachineMemOperand::MOStore,
      MFI.getObjectSize(FI), MFI.getObjectAlign(FI));
  // STIDX64 data, base, offset — base/offset filled in by eliminateFrameIndex.
  BuildMI(MBB, I, DebugLoc(), get(KlaussCPU::STIDX64))
      .addReg(SrcReg, getKillRegState(IsKill))
      .addFrameIndex(FI)
      .addImm(0)
      .addMemOperand(MMO)
      .setMIFlag(Flags);
}

void KlaussCPUInstrInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
                                               MachineBasicBlock::iterator I,
                                               Register DstReg, int FI,
                                               const TargetRegisterClass *RC,
                                               Register VReg,
                                               unsigned SubReg,
                                               MachineInstr::MIFlag Flags) const {
  assert(KlaussCPU::GPRRegClass.hasSubClassEq(RC) &&
         "KlaussCPU loadRegFromStackSlot: only GPR spills supported");
  MachineFunction &MF = *MBB.getParent();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  MachineMemOperand *MMO = MF.getMachineMemOperand(
      MachinePointerInfo::getFixedStack(MF, FI), MachineMemOperand::MOLoad,
      MFI.getObjectSize(FI), MFI.getObjectAlign(FI));
  // LDIDX64 dst, base, offset — base/offset filled in by eliminateFrameIndex.
  BuildMI(MBB, I, DebugLoc(), get(KlaussCPU::LDIDX64), DstReg)
      .addFrameIndex(FI)
      .addImm(0)
      .addMemOperand(MMO)
      .setMIFlag(Flags);
}

unsigned KlaussCPUInstrInfo::getCondBranchOpcode(ISD::CondCode CC, bool PIC) {
  // Post flag-unification (RTL fbb77d7 / FLAG_UNIFICATION_CHANGES) there is a
  // single Z/S/C/V flags register: CMP is SUB-without-writeback and each COND
  // is derived in hardware (EQ=Z, signed LT=S^V, unsigned ULT=C, ...).
  // BORROW POLARITY (x86, NOT ARM): after SUB/CMP, unsigned `<` is C and
  // unsigned `>=` is ¬C.  SETULT→JMPULT / SETUGE→JMPUGE encode this; do NOT
  // invert it in any future carry-based (ADC/SBC / setcc-from-carry) lowering.
  switch (CC) {
  case ISD::SETEQ:  return PIC ? KlaussCPU::JMPEREL   : KlaussCPU::JMPE;
  case ISD::SETNE:  return PIC ? KlaussCPU::JMPNEREL  : KlaussCPU::JMPNE;
  case ISD::SETLT:  return PIC ? KlaussCPU::JMPLTREL  : KlaussCPU::JMPLT;
  case ISD::SETLE:  return PIC ? KlaussCPU::JMPLEREL  : KlaussCPU::JMPLE;
  case ISD::SETGT:  return PIC ? KlaussCPU::JMPGTREL  : KlaussCPU::JMPGT;
  case ISD::SETGE:  return PIC ? KlaussCPU::JMPGEREL  : KlaussCPU::JMPGE;
  case ISD::SETULT: return PIC ? KlaussCPU::JMPULTREL : KlaussCPU::JMPULT;
  case ISD::SETULE: return PIC ? KlaussCPU::JMPULEREL : KlaussCPU::JMPULE;
  case ISD::SETUGT: return PIC ? KlaussCPU::JMPUGTREL : KlaussCPU::JMPUGT;
  case ISD::SETUGE: return PIC ? KlaussCPU::JMPUGEREL : KlaussCPU::JMPUGE;
  default:          return 0;
  }
}

// Conditional-branch opcode pairs: {branch, inverse}.  Covers the absolute
// and PC-relative (PIC) forms.  Used by analyzeBranch (to recognise a
// conditional branch) and reverseBranchCondition.
static const unsigned CondBranchInverse[][2] = {
    {KlaussCPU::JMPZ, KlaussCPU::JMPNZ},     {KlaussCPU::JMPE, KlaussCPU::JMPNE},
    {KlaussCPU::JMPC, KlaussCPU::JMPNC},     {KlaussCPU::JMPS, KlaussCPU::JMPNS},
    {KlaussCPU::JMPO, KlaussCPU::JMPNO},     {KlaussCPU::JMPLT, KlaussCPU::JMPGE},
    {KlaussCPU::JMPLE, KlaussCPU::JMPGT},    {KlaussCPU::JMPULT, KlaussCPU::JMPUGE},
    {KlaussCPU::JMPULE, KlaussCPU::JMPUGT},
    {KlaussCPU::JMPZREL, KlaussCPU::JMPNZREL},
    {KlaussCPU::JMPEREL, KlaussCPU::JMPNEREL},
    {KlaussCPU::JMPCREL, KlaussCPU::JMPNCREL},
    {KlaussCPU::JMPSREL, KlaussCPU::JMPNSREL},
    {KlaussCPU::JMPLTREL, KlaussCPU::JMPGEREL},
    {KlaussCPU::JMPLEREL, KlaussCPU::JMPGTREL},
    {KlaussCPU::JMPULTREL, KlaussCPU::JMPUGEREL},
    {KlaussCPU::JMPULEREL, KlaussCPU::JMPUGTREL},
};

// Inverse of a conditional branch opcode, or 0 if Opc is not one.
static unsigned getInverseCondBranch(unsigned Opc) {
  for (const auto &P : CondBranchInverse) {
    if (P[0] == Opc)
      return P[1];
    if (P[1] == Opc)
      return P[0];
  }
  return 0;
}

// A direct (immediate-target) branch: JMP/JMPREL or any conditional JMPxx.
// Indirect JMPR_R and returns are terminators but not direct branches.
static bool isDirectBranch(const MachineInstr &MI) {
  return MI.getDesc().isBranch() && !MI.getDesc().isIndirectBranch();
}

bool KlaussCPUInstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                        MachineBasicBlock *&TBB,
                                        MachineBasicBlock *&FBB,
                                        SmallVectorImpl<MachineOperand> &Cond,
                                        bool AllowModify) const {
  // Cond encoding: a single immediate holding the conditional JMPxx opcode.
  // The flag-setting compare stays in the block — removeBranch/insertBranch
  // only touch the jumps, so the compare/branch pairing survives rewrites.
  // (Modelled on RISCVInstrInfo::analyzeBranch.)
  TBB = FBB = nullptr;
  Cond.clear();

  MachineBasicBlock::iterator I = MBB.getLastNonDebugInstr();
  if (I == MBB.end() || !isUnpredicatedTerminator(*I))
    return false; // falls through

  // Count terminators; find the first unconditional or indirect branch.
  MachineBasicBlock::iterator FirstUncondOrIndirect = MBB.end();
  int NumTerminators = 0;
  for (auto J = I.getReverse(); J != MBB.rend() && isUnpredicatedTerminator(*J);
       ++J) {
    // ISA v3 B fused branches (pre-emit only) carry their target at
    // operand 3 and no separate compare: not modelled here.
    if (J->getOpcode() == KlaussCPU::FBR_RR || J->getOpcode() == KlaussCPU::FBR_RI)
      return true;
    ++NumTerminators;
    if (J->getDesc().isUnconditionalBranch() ||
        J->getDesc().isIndirectBranch())
      FirstUncondOrIndirect = J.getReverse();
  }

  // Anything after an unconditional/indirect branch is dead.
  if (AllowModify && FirstUncondOrIndirect != MBB.end()) {
    while (std::next(FirstUncondOrIndirect) != MBB.end()) {
      std::next(FirstUncondOrIndirect)->eraseFromParent();
      --NumTerminators;
    }
    I = FirstUncondOrIndirect;
  }

  // Returns, indirect branches (JMPR_R) and anything else we don't model.
  if (!isDirectBranch(*I) || NumTerminators > 2)
    return true;

  if (NumTerminators == 1) {
    TBB = I->getOperand(0).getMBB();
    if (I->getDesc().isConditionalBranch())
      Cond.push_back(MachineOperand::CreateImm(I->getOpcode()));
    return false;
  }

  // Two terminators: conditional followed by unconditional.
  MachineBasicBlock::iterator P = std::prev(I);
  if (P->getDesc().isConditionalBranch() && isDirectBranch(*P) &&
      I->getDesc().isUnconditionalBranch()) {
    TBB = P->getOperand(0).getMBB();
    Cond.push_back(MachineOperand::CreateImm(P->getOpcode()));
    FBB = I->getOperand(0).getMBB();
    return false;
  }
  return true;
}

bool KlaussCPUInstrInfo::reverseBranchCondition(
    SmallVectorImpl<MachineOperand> &Cond) const {
  assert(Cond.size() == 1 && "Invalid KlaussCPU branch condition");
  unsigned Inv = getInverseCondBranch(Cond[0].getImm());
  if (!Inv)
    return true;
  Cond[0].setImm(Inv);
  return false;
}

unsigned KlaussCPUInstrInfo::removeBranch(MachineBasicBlock &MBB,
                                           int *BytesRemoved) const {
  // Remove at most an unconditional branch and the conditional before it.
  unsigned Count = 0;
  int Bytes = 0;
  MachineBasicBlock::iterator I = MBB.getLastNonDebugInstr();
  while (I != MBB.end() && Count < 2 && isDirectBranch(*I)) {
    Bytes += I->getDesc().getSize();
    I->eraseFromParent();
    ++Count;
    I = MBB.getLastNonDebugInstr();
  }
  if (BytesRemoved)
    *BytesRemoved = Bytes;
  return Count;
}

unsigned KlaussCPUInstrInfo::insertBranch(MachineBasicBlock &MBB,
                                           MachineBasicBlock *TBB,
                                           MachineBasicBlock *FBB,
                                           ArrayRef<MachineOperand> Cond,
                                           const DebugLoc &DL,
                                           int *BytesAdded) const {
  assert(TBB && "insertBranch must not be called with null TBB");

  // Use JMPREL (PC-relative) for unconditional branches in PIC mode so the
  // binary can be relocated correctly when loaded at a non-zero delta.
  unsigned UncondOpc = STI.isPositionIndependent() ? KlaussCPU::JMPREL
                                                    : KlaussCPU::JMP;
  unsigned Count = 0;
  int Bytes = 0;

  if (Cond.empty()) {
    // Unconditional branch.
    assert(!FBB && "Unexpected FBB with unconditional branch");
    BuildMI(&MBB, DL, get(UncondOpc)).addMBB(TBB);
    Count = 1;
    Bytes = 8;
  } else {
    // Conditional branch: Cond[0] holds the JMPxx / JMPxxREL opcode.
    // The preceding CMPRR/CMPRV is already in the block (removeBranch only
    // removes branch instructions, not compares) so we just emit the jump.
    assert(Cond.size() == 1 && "Expected exactly one Cond operand");
    unsigned CondOpc = Cond[0].getImm();
    BuildMI(&MBB, DL, get(CondOpc)).addMBB(TBB);
    Count = 1;
    Bytes = 8;
    if (FBB) {
      BuildMI(&MBB, DL, get(UncondOpc)).addMBB(FBB);
      Count++;
      Bytes += 8;
    }
  }

  if (BytesAdded)
    *BytesAdded = Bytes;
  return Count;
}

void KlaussCPUInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                                      MachineBasicBlock::iterator I,
                                      const DebugLoc &DL,
                                      Register DestReg, Register SrcReg,
                                      bool KillSrc, bool RenamableDest,
                                      bool RenamableSrc) const {
  // SP ↔ GPR: use GETSP_R / SETSP_R.
  if (SrcReg == KlaussCPU::SP) {
    assert(KlaussCPU::GPRRegClass.contains(DestReg));
    BuildMI(MBB, I, DL, get(KlaussCPU::GETSP_R), DestReg);
    return;
  }
  if (DestReg == KlaussCPU::SP) {
    assert(KlaussCPU::GPRRegClass.contains(SrcReg));
    BuildMI(MBB, I, DL, get(KlaussCPU::SETSP_R))
        .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }
  // GPR → GPR.
  assert(KlaussCPU::GPRRegClass.contains(DestReg, SrcReg) &&
         "KlaussCPU copyPhysReg: only GPR↔GPR or SP↔GPR copies supported");
  BuildMI(MBB, I, DL, get(KlaussCPU::COPY_R), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc));
}

