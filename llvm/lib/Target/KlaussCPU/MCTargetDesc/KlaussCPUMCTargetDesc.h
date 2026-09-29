//===-- KlaussCPUMCTargetDesc.h - KlaussCPU Target Descriptions ---*- C++ -*-===//
//
// KlaussCPU LLVM backend — MC target descriptor declarations.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUMCTARGETDESC_H
#define LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUMCTARGETDESC_H

#include <cstdint>

namespace llvm {
class MCAsmBackend;
class MCCodeEmitter;
class MCContext;
class MCInst;
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
/// ISA v3: rewrite a 2-word immediate instruction into its 1-word short form
/// when the immediate fits (KlaussCPUMCCompress.cpp). Returns true if changed.
bool compressKlaussCPUInst(MCInst &Inst);
/// ISA v3 A1 short branches (KlaussCPUMCCompress.cpp).
bool isKlaussCPUShortBranch(unsigned Opcode);
/// The 2-word form a short branch relaxes to (PC-relative when one exists).
unsigned relaxKlaussCPUShortBranch(unsigned Opcode);
/// Word-0 template of a short branch (displacement bits zero).
uint32_t klaussCPUShortBranchWord(unsigned Opcode);
} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUMCTARGETDESC_H
