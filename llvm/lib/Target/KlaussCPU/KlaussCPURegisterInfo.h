//===-- KlaussCPURegisterInfo.h - KlaussCPU Register Info -------*- C++ -*-===//
//
// KlaussCPU LLVM backend — register info class declaration.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUREGISTERINFO_H
#define LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUREGISTERINFO_H

#include "llvm/CodeGen/TargetRegisterInfo.h"

#define GET_REGINFO_ENUM
#define GET_REGINFO_HEADER
#include "KlaussCPUGenRegisterInfo.inc"

namespace llvm {

class KlaussCPURegisterInfo : public KlaussCPUGenRegisterInfo {
public:
  KlaussCPURegisterInfo();

  const MCPhysReg *getCalleeSavedRegs(const MachineFunction *MF) const override;

  const uint32_t *getCallPreservedMask(const MachineFunction &MF,
                                       CallingConv::ID CC) const override;

  BitVector getReservedRegs(const MachineFunction &MF) const override;

  // Enable the real register scavenger so eliminateFrameIndex receives a
  // non-null RS for sub-word frame-slot access (MEMGET8/16, MEMSET8/16).
  // requiresFrameIndexScavenging must remain false (the default) — returning
  // true there switches PEI into virtual-register mode, which passes nullptr
  // to eliminateFrameIndex instead.
  bool requiresRegisterScavenging(const MachineFunction &MF) const override {
    return true;
  }

  bool eliminateFrameIndex(MachineBasicBlock::iterator II, int SPAdj,
                           unsigned FIOperandNum,
                           RegScavenger *RS = nullptr) const override;

  Register getFrameRegister(const MachineFunction &MF) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUREGISTERINFO_H
