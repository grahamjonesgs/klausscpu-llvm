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
  MachineBasicBlock::iterator I = MBB.end();
  if (I == MBB.begin())
    return false; // empty block — fall through

  // Skip debug instructions.
  while (I != MBB.begin() && std::prev(I)->isDebugInstr())
    --I;
  if (I == MBB.begin())
    return false;

  --I;
  // Only handle a single trailing unconditional JMP.
  if (I->getOpcode() == KlaussCPU::JMP) {
    TBB = I->getOperand(0).getMBB();
    return false;
  }

  // Anything else (conditional branches, non-branch) we cannot analyze.
  return true;
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
  // Only unconditional branches are supported here.
  // Conditional branch sequences (CMPRR/CMPRV + JMPxx) are multi-instruction
  // and must be inserted by the code generator, not the branch folder.
  assert(TBB && "insertBranch must not be called with null TBB");
  assert(Cond.empty() && !FBB &&
         "KlaussCPU: only unconditional branches in insertBranch");
  BuildMI(&MBB, DL, get(KlaussCPU::JMP)).addMBB(TBB);
  if (BytesAdded)
    *BytesAdded = 8;
  return 1;
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

