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
/// R_KLAUSSCPU_TPREL32 (TLS local exec, variant II: TLS below TP).  The
/// dynamic-linking ones (ABI.md "Dynamic linking") take a bare symbol in the
/// word-1 immediate of a 2-word PC-relative instruction: `%gotpcrel(sym)`,
/// `%gottprel(sym)` and `%tlsgd(sym)` in `leapc` (the symbol's GOT entry,
/// its initial-exec TP-offset entry, its general-dynamic GOT pair), and
/// `%plt(sym)` in `callrel` / `jmprel` / `tail` (its PLT entry).
enum Specifier : uint16_t {
  S_None = 0,
  S_TPREL = 1,
  S_GOTPCREL = 2,
  S_PLT = 3,
  S_GOTTPREL = 4,
  S_TLSGD = 5,
};

/// The assembler spelling of a specifier, without the '%': "tprel", ...
StringRef getSpecifierName(uint16_t S);
/// The specifier of a spelling, in any case, or S_None.
uint16_t parseSpecifierName(StringRef Name);
/// True for the dynamic-linking specifiers, which go only in the word-1
/// immediate of a 2-word PC-relative instruction: a `leapc` (%gotpcrel,
/// %gottprel, %tlsgd) or `callrel` / `jmprel` / `tail` (%plt).  An
/// instruction that carries one is never compressed or relaxed.
inline bool isPCRelSpecifier(uint16_t S) {
  return S == S_GOTPCREL || S == S_PLT || S == S_GOTTPREL || S == S_TLSGD;
}
/// Whether instruction Opcode takes PC-relative specifier S in its word-1
/// immediate (see isPCRelSpecifier).
bool isSpecifierAllowed(unsigned Opcode, uint16_t S);

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
  /// Dynamic linking (Linux PIC): `%gotpcrel(sym)`, `%plt(sym)`,
  /// `%gottprel(sym)`, `%tlsgd(sym)`.
  MO_GOTPCREL = 2,
  MO_PLT = 3,
  MO_GOTTPREL = 4,
  MO_TLSGD = 5,
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
