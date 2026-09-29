//===-- KlaussCPUInstrInfo.h - KlaussCPU Instruction Information --*- C++ -*-===//
//
// KlaussCPU LLVM backend — instruction info class declaration.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUINSTRINFO_H
#define LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUINSTRINFO_H

#include "llvm/CodeGen/ISDOpcodes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

// GET_INSTRINFO_ENUM must appear before GET_INSTRINFO_HEADER so that the
// instruction opcode enum (KlaussCPU::PUSH_R, KlaussCPU::ADDR, etc.) is
// visible to code that includes this header.
#define GET_INSTRINFO_ENUM
#define GET_INSTRINFO_HEADER
#include "KlaussCPUGenInstrInfo.inc"

namespace llvm {

namespace KlaussCPU {
// ISA v3 D2: set in a SELECT_CC_RR/RI pseudo's $jmp immediate (above any
// opcode number) to make the custom inserter emit CMPRRW/CMPRVW.
constexpr unsigned SelectCCW32Flag = 1u << 31;
} // namespace KlaussCPU

class KlaussCPURegisterInfo;
class KlaussCPUSubtarget;

class KlaussCPUInstrInfo : public KlaussCPUGenInstrInfo {
  const KlaussCPUSubtarget &STI;

public:
  explicit KlaussCPUInstrInfo(const KlaussCPUSubtarget &STI,
                               const KlaussCPURegisterInfo &RI);

  // Conditional jump taken when `lhs CC rhs` holds after CMPRR/CMPRV lhs,rhs
  // (JMPxxREL in PIC mode).  Returns 0 for a CondCode with no direct jump.
  static unsigned getCondBranchOpcode(ISD::CondCode CC, bool PIC);

  // getCallFrameSetupOpcode / getCallFrameDestroyOpcode are NOT virtual in
  // LLVM 23.  The opcodes are stored in TargetInstrInfo::CallFrameSetupOpcode /
  // CallFrameDestroyOpcode and set via the KlaussCPUGenInstrInfo constructor
  // parameters — see KlaussCPUInstrInfo.cpp.

  // Branch analysis.  Cond = {Imm(JMPxx opcode)}; the flag-setting compare
  // is left in place and is never touched by remove/insertBranch.
  bool analyzeBranch(MachineBasicBlock &MBB, MachineBasicBlock *&TBB,
                     MachineBasicBlock *&FBB,
                     SmallVectorImpl<MachineOperand> &Cond,
                     bool AllowModify = false) const override;

  bool reverseBranchCondition(
      SmallVectorImpl<MachineOperand> &Cond) const override;

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
