//===-- KlaussCPUMCTargetDesc.h - KlaussCPU Target Descriptions ---*- C++ -*-===//
//
// KlaussCPU LLVM backend — MC target descriptor declarations.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUMCTARGETDESC_H
#define LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUMCTARGETDESC_H

namespace llvm {
class MCAsmBackend;
class MCCodeEmitter;
class MCContext;
class MCInstrInfo;
class MCRegisterInfo;
class MCSubtargetInfo;
class MCTargetOptions;
class Target;

MCCodeEmitter *createKlaussCPUMCCodeEmitter(const MCInstrInfo &MCII,
                                             MCContext &Ctx);

MCAsmBackend *createKlaussCPUAsmBackend(const Target &T,
                                         const MCSubtargetInfo &STI,
                                         const MCRegisterInfo &MRI,
                                         const MCTargetOptions &Options);
} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUMCTARGETDESC_H
