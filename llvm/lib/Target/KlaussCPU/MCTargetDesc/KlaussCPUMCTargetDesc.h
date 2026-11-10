//===-- KlaussCPUMCTargetDesc.h - KlaussCPU Target Descriptions ---*- C++ -*-===//
//
// KlaussCPU LLVM backend — MC target descriptor declarations.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUMCTARGETDESC_H
#define LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUMCTARGETDESC_H

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <optional>
#include <string>

namespace llvm {
class MCAsmBackend;
class MCCodeEmitter;
class MCContext;
class MCExpr;
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

/// System registers of MFSR / MTSR (klaussCPU MMU_PLAN.md §6.1, §10.1,
/// §11.3): the assembler name of SR number Num (lower case), or an empty
/// StringRef for a number that has none.
StringRef getSysRegName(unsigned Num);
/// The SR number of a name, in any case (`ecause`, `ECAUSE`).
std::optional<unsigned> lookupSysReg(StringRef Name);

/// SR number of TP, the thread pointer (user-readable and -writable).
constexpr unsigned SysRegTP = 0x07;

/// Relocation specifiers (MCSpecifierExpr).  S_TPREL, `%tprel(sym)`: the
/// offset of TLS symbol sym from the thread pointer, FK_KlaussCPU_TPREL32 /
/// R_KLAUSSCPU_TPREL32 (TLS local exec, variant II: TLS below TP).
enum Specifier : uint16_t {
  S_None = 0,
  S_TPREL = 1,
};

/// The relocation specifier anywhere in Expr (`%tprel(x)`, `%tprel(x)+4`),
/// or S_None.  It does not need an assembler, unlike evaluateAsRelocatable,
/// so the asm parser and the encoder can classify an operand with it.
uint16_t getExprSpecifier(const MCExpr *Expr);
} // namespace KlaussCPU

/// MachineOperand target flags.
namespace KlaussCPUII {
enum : unsigned {
  MO_None = 0,
  /// A TLS global's offset from the thread pointer, printed `%tprel(sym)`.
  MO_TPREL = 1,
};
} // namespace KlaussCPUII
} // namespace llvm

// Defines symbolic names for the subtarget features.
#define GET_SUBTARGETINFO_ENUM
#include "KlaussCPUGenSubtargetInfo.inc"

// Declares KlaussCPU_MC::verifyInstructionPredicates.
#define GET_INSTRINFO_MC_HELPER_DECLS
#include "KlaussCPUGenInstrInfo.inc"

#endif // LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUMCTARGETDESC_H
