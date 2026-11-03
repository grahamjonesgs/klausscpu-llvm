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

  /// Pointers are 64-bit GPRs.  Needed by passes that constrain the address
  /// register of an inline-asm memory operand ("m"), e.g. Machine Copy
  /// Propagation forwarding a copy into one.
  const TargetRegisterClass *
  getPointerRegClass(unsigned Kind = 0) const override {
    return &KlaussCPU::GPRRegClass;
  }

  bool shouldRealignStack(const MachineFunction &MF) const override;
  bool canRealignStack(const MachineFunction &MF) const override;

  bool isAsmClobberable(const MachineFunction &MF,
                        MCRegister PhysReg) const override;
  bool isInlineAsmReadOnlyReg(const MachineFunction &MF,
                              MCRegister PhysReg) const override;

  // Keep the register scavenger so eliminateFrameIndex has a non-null RS.
  // Isel puts a frame index only where eliminateFrameIndex folds it into a
  // base+offset operand, so a scratch register is needed only for a frame
  // over 1.5 GiB (or a frame index in an operand that is not a base).
  // requiresFrameIndexScavenging must remain false (the default): true
  // switches PEI into virtual-register mode, which passes nullptr to
  // eliminateFrameIndex instead.
  bool requiresRegisterScavenging(const MachineFunction &MF) const override {
    return true;
  }

  bool eliminateFrameIndex(MachineBasicBlock::iterator II, int SPAdj,
                           unsigned FIOperandNum,
                           RegScavenger *RS = nullptr) const override;

  Register getFrameRegister(const MachineFunction &MF) const override;

  // R15, the frame pointer, is not in CSR_KlaussCPU (the prologue saves it),
  // but an instruction that reads or writes it must stay inside a
  // shrink-wrapped frame.
  bool isNonallocatableRegisterCalleeSave(MCRegister Reg) const override {
    return Reg == KlaussCPU::R15;
  }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUREGISTERINFO_H
