//===-- KlaussCPUTargetInfo.h - KlaussCPU Target Info -----------*- C++ -*-===//
//
// KlaussCPU LLVM backend — target info declaration.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_KLAUSSCPU_TARGETINFO_KLAUSSCPUTARGETINFO_H
#define LLVM_LIB_TARGET_KLAUSSCPU_TARGETINFO_KLAUSSCPUTARGETINFO_H

namespace llvm {
class Target;
Target &getTheKlaussCPUTarget();
} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_TARGETINFO_KLAUSSCPUTARGETINFO_H
