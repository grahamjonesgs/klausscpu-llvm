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
public:
  explicit KlaussCPUInstrInfo(const KlaussCPUSubtarget &STI,
                               const KlaussCPURegisterInfo &RI);
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUINSTRINFO_H
