//===-- KlaussCPUInstrInfo.h - KlaussCPU Instruction Information --*- C++ -*-===//
//
// KlaussCPU LLVM backend — instruction info class declaration.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUINSTRINFO_H
#define LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUINSTRINFO_H

#include "llvm/CodeGen/TargetInstrInfo.h"

// GET_INSTRINFO_ENUM must appear before GET_INSTRINFO_HEADER so that the
// instruction opcode enum (KlaussCPU::PUSH_R, KlaussCPU::ADDR, etc.) is
// visible to code that includes this header.
#define GET_INSTRINFO_ENUM
#define GET_INSTRINFO_HEADER
#include "KlaussCPUGenInstrInfo.inc"

namespace llvm {

class KlaussCPURegisterInfo;
class KlaussCPUSubtarget;

class KlaussCPUInstrInfo : public KlaussCPUGenInstrInfo {
  const KlaussCPUSubtarget &STI;

public:
  explicit KlaussCPUInstrInfo(const KlaussCPUSubtarget &STI,
                               const KlaussCPURegisterInfo &RI);

  // getCallFrameSetupOpcode / getCallFrameDestroyOpcode are NOT virtual in
  // LLVM 23.  The opcodes are stored in TargetInstrInfo::CallFrameSetupOpcode /
  // CallFrameDestroyOpcode and set via the KlaussCPUGenInstrInfo constructor
  // parameters — see KlaussCPUInstrInfo.cpp.

  // Branch analysis: recognize JMP (unconditional) at the tail of a block.
  // Conditional branches (CMPRR/CMPRV + JMPxx) return true (not analyzed)
  // since they are two-instruction sequences.
  bool analyzeBranch(MachineBasicBlock &MBB, MachineBasicBlock *&TBB,
                     MachineBasicBlock *&FBB,
                     SmallVectorImpl<MachineOperand> &Cond,
                     bool AllowModify = false) const override;

  unsigned removeBranch(MachineBasicBlock &MBB,
                        int *BytesRemoved = nullptr) const override;

  unsigned insertBranch(MachineBasicBlock &MBB, MachineBasicBlock *TBB,
                        MachineBasicBlock *FBB,
                        ArrayRef<MachineOperand> Cond, const DebugLoc &DL,
                        int *BytesAdded = nullptr) const override;

  void copyPhysReg(MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
                   const DebugLoc &DL, Register DestReg, Register SrcReg,
                   bool KillSrc, bool RenamableDest = false,
                   bool RenamableSrc = false) const override;

  // LLVM 23 signatures: no TRI parameter; VReg follows RC; load has SubReg+Flags.
  void storeRegToStackSlot(MachineBasicBlock &MBB,
                            MachineBasicBlock::iterator I,
                            Register SrcReg, bool IsKill, int FI,
                            const TargetRegisterClass *RC,
                            Register VReg,
                            MachineInstr::MIFlag Flags =
                                MachineInstr::NoFlags) const override;

  void loadRegFromStackSlot(MachineBasicBlock &MBB,
                             MachineBasicBlock::iterator I,
                             Register DstReg, int FI,
                             const TargetRegisterClass *RC,
                             Register VReg,
                             unsigned SubReg = 0,
                             MachineInstr::MIFlag Flags =
                                 MachineInstr::NoFlags) const override;

};

} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUINSTRINFO_H
