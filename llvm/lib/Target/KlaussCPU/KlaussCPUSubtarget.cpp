//===-- KlaussCPUSubtarget.cpp - KlaussCPU Subtarget Information ----------===//
//
// KlaussCPU LLVM backend — subtarget implementation.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUSubtarget.h"
#include "MCTargetDesc/KlaussCPUMCTargetDesc.h"

#define DEBUG_TYPE "klausscpu-subtarget"

#define GET_SUBTARGETINFO_TARGET_DESC
#define GET_SUBTARGETINFO_CTOR
#include "KlaussCPUGenSubtargetInfo.inc"

using namespace llvm;

KlaussCPUSubtarget::KlaussCPUSubtarget(const Triple &TT, StringRef CPU,
                                        StringRef FS, const TargetMachine &TM)
    : KlaussCPUGenSubtargetInfo(TT, CPU, /*TuneCPU=*/CPU,
                                KlaussCPU::getEffectiveFeatureString(FS)),
      RI(),
      II(*this, RI),  // RI is initialized before II (declaration order)
      FL(*this),
      TLI(TM, *this),
      TSI(),
      TM(TM) {
  // Parse any subtarget features.
  std::string CPUName = CPU.empty() ? "generic" : CPU.str();
  ParseSubtargetFeatures(CPUName, CPUName, getFeatureString());
}

bool KlaussCPUSubtarget::isPositionIndependent() const {
  return TM.isPositionIndependent();
}
