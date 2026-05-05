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

KlaussCPUInstrInfo::KlaussCPUInstrInfo(const KlaussCPUSubtarget &STI,
                                        const KlaussCPURegisterInfo &RI)
    : KlaussCPUGenInstrInfo(STI, RI,
                            KlaussCPU::ADJCALLSTACKDOWN,
                            KlaussCPU::ADJCALLSTACKUP) {}

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

// Return true if MI is any branch instruction (unconditional or conditional).
static bool isBranchOpcode(unsigned Opc) {
  switch (Opc) {
  case KlaussCPU::JMP:
  case KlaussCPU::JMPE:  case KlaussCPU::JMPNE:
  case KlaussCPU::JMPLT: case KlaussCPU::JMPLE:
  case KlaussCPU::JMPGT: case KlaussCPU::JMPGE:
  case KlaussCPU::JMPULT: case KlaussCPU::JMPULE:
  case KlaussCPU::JMPUGT: case KlaussCPU::JMPUGE:
    return true;
  default:
    return false;
  }
}

bool KlaussCPUInstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                        MachineBasicBlock *&TBB,
                                        MachineBasicBlock *&FBB,
                                        SmallVectorImpl<MachineOperand> &Cond,
                                        bool AllowModify) const {
  // BranchFolder uses AllowModify=true.  Our conditional branches are
  // CMPRR/V + JMPxx pairs; insertBranch only supports unconditional JMP, so
  // we refuse analysis when modification is requested.
  if (AllowModify)
    return true;

  // Read-only analysis (MachineBlockPlacement, canFallThrough, …).
  // Scan backward to identify the last branch(es).
  MachineBasicBlock::reverse_iterator I = MBB.rbegin();
  while (I != MBB.rend() && I->isDebugInstr())
    ++I;

  if (I == MBB.rend() || !isBranchOpcode(I->getOpcode()))
    return false; // no branches

  if (I->getOpcode() == KlaussCPU::JMP) {
    // Unconditional branch — may be preceded by a conditional one.
    TBB = I->getOperand(0).getMBB();
    ++I;
    while (I != MBB.rend() && I->isDebugInstr())
      ++I;
    if (I != MBB.rend() && isBranchOpcode(I->getOpcode()) &&
        I->getOpcode() != KlaussCPU::JMP) {
      // Conditional branch followed by unconditional: TBB = cond target, FBB = uncond.
      FBB = TBB;
      TBB = I->getOperand(0).getMBB();
      Cond.push_back(MachineOperand::CreateImm(I->getOpcode()));
    }
  } else {
    // Conditional branch with fallthrough.
    TBB = I->getOperand(0).getMBB();
    Cond.push_back(MachineOperand::CreateImm(I->getOpcode()));
  }
  return false;
}

unsigned KlaussCPUInstrInfo::removeBranch(MachineBasicBlock &MBB,
                                           int *BytesRemoved) const {
  MachineBasicBlock::iterator I = MBB.end();
  unsigned Count = 0;
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr())
      continue;
    if (!isBranchOpcode(I->getOpcode()))
      break;
    I->eraseFromParent();
    I = MBB.end();
    ++Count;
  }
  if (BytesRemoved)
    *BytesRemoved = Count * 8; // every branch instruction is 8 bytes
  return Count;
}

unsigned KlaussCPUInstrInfo::insertBranch(MachineBasicBlock &MBB,
                                           MachineBasicBlock *TBB,
                                           MachineBasicBlock *FBB,
                                           ArrayRef<MachineOperand> Cond,
                                           const DebugLoc &DL,
                                           int *BytesAdded) const {
  assert(TBB && "insertBranch must not be called with null TBB");
  unsigned Count = 0;
  int Bytes = 0;

  if (Cond.empty()) {
    // Unconditional branch.
    assert(!FBB && "Unexpected FBB with unconditional branch");
    BuildMI(&MBB, DL, get(KlaussCPU::JMP)).addMBB(TBB);
    Count = 1;
    Bytes = 8;
  } else {
    // Conditional branch: Cond[0] holds the JMPxx opcode.
    // The preceding CMPRR/CMPRV is already in the block (removeBranch only
    // removes branch instructions, not compares) so we just emit the jump.
    assert(Cond.size() == 1 && "Expected exactly one Cond operand");
    unsigned CondOpc = Cond[0].getImm();
    BuildMI(&MBB, DL, get(CondOpc)).addMBB(TBB);
    Count = 1;
    Bytes = 8;
    if (FBB) {
      BuildMI(&MBB, DL, get(KlaussCPU::JMP)).addMBB(FBB);
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

