//===-- KlaussCPUMCTargetDesc.h - KlaussCPU Target Descriptions ---*- C++ -*-===//
//
// KlaussCPU LLVM backend — MC target descriptor declarations.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUMCTARGETDESC_H
#define LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUMCTARGETDESC_H

#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCInstrDesc.h"
#include <string>

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
namespace KlaussCPU {
/// FS with the -klausscpu-short-forms / -klausscpu-short-branches overrides
/// applied, for every subtarget the target creates.
std::string getEffectiveFeatureString(StringRef FS);

/// The CompressPats (KlaussCPUInstrInfo.td): rewrite MI into its ISA v3
/// short 1-word form, or a short branch back into its 2-word form.  Return
/// false, leaving OutInst untouched, when MI has no such form.
bool compress(MCInst &OutInst, const MCInst &MI, const MCSubtargetInfo &STI);
bool uncompress(MCInst &OutInst, const MCInst &MI,
                const MCSubtargetInfo &STI);
} // namespace KlaussCPU

namespace KlaussCPUII {
/// TSFlags bits (KCInst in KlaussCPUInstrInfo.td).
enum { ActsInEXShadowMask = 1 << 0 };

/// True for an instruction whose effect a squash in EX does not undo: it
/// starts the RTL divider (DIVR/DIVUR/MODR/MODUR/DIVV/MODV) or strobes the
/// LCD (LCDCMDV/LCDDATAV/LCDDATAR) from EX.  A taken fused branch (FBR)
/// redirects from MEM and squashes the instruction behind it in EX, so such
/// an instruction must not follow an FBR directly (KlaussCPUFuseCmpBr.cpp).
inline bool actsInEXShadow(const MCInstrDesc &Desc) {
  return Desc.TSFlags & ActsInEXShadowMask;
}
} // namespace KlaussCPUII
} // namespace llvm

// Defines symbolic names for the subtarget features.
#define GET_SUBTARGETINFO_ENUM
#include "KlaussCPUGenSubtargetInfo.inc"

// Declares KlaussCPU_MC::verifyInstructionPredicates.
#define GET_INSTRINFO_MC_HELPER_DECLS
#include "KlaussCPUGenInstrInfo.inc"

#endif // LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUMCTARGETDESC_H
