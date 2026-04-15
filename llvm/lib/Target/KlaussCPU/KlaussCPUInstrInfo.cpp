//===-- KlaussCPUInstrInfo.cpp - KlaussCPU Instruction Information --------===//
//
// KlaussCPU LLVM backend — instruction info implementation.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUInstrInfo.h"
#include "KlaussCPURegisterInfo.h"
#include "KlaussCPUSubtarget.h"

#define GET_INSTRINFO_CTOR_DTOR
#include "KlaussCPUGenInstrInfo.inc"

using namespace llvm;

KlaussCPUInstrInfo::KlaussCPUInstrInfo(const KlaussCPUSubtarget &STI,
                                        const KlaussCPURegisterInfo &RI)
    : KlaussCPUGenInstrInfo(STI, RI) {}
