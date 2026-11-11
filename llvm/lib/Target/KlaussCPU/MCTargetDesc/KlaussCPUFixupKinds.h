//===-- KlaussCPUFixupKinds.h - KlaussCPU fixup entry definitions ----------===//
//
// FK_KlaussCPU_ABS32   — 32-bit absolute address, word 1 (bytes [4:7]) of an
//                         8-byte branch/call/SETR(U) instruction. LE order.
// FK_KlaussCPU_PCREL32 — 32-bit signed PC-relative offset, word 1 of an 8-byte
//                         JMPREL/JMPxxREL/CALLREL/LEAPC instruction.
//                         field_value = target - PC_of_instruction.
//                         (PC_of_instruction = instr_addr, NOT instr_addr+4.)
// FK_KlaussCPU_PCREL18 / FK_KlaussCPU_CALL18 — simm18 word displacement in
//                         word0[17:0] of a short branch / call.
// FK_KlaussCPU_PCREL13 — simm13 word displacement in word0[20:8] (FBR).
// FK_KlaussCPU_PCREL16 — simm16 byte displacement in word0[19:12] (high
//                         byte) and word0[7:0] (low byte) of a LEAPC.S.
// FK_KlaussCPU_TPREL32 — 32-bit signed offset of a TLS symbol from the thread
//                         pointer (`%tprel(sym)`), word 1 of an 8-byte
//                         ADDI / LDIDX* / STIDX* / SETR: R_KLAUSSCPU_TPREL32.
// FK_KlaussCPU_GOTPCREL32 / _PLT32 / _TLS_GOTTPREL32 / _TLS_GD32 — dynamic
//                         linking: `%gotpcrel` / `%plt` / `%gottprel` /
//                         `%tlsgd` in word 1 of a 2-word LEAPC or
//                         CALLREL / JMPREL / TAILREL: relocations 8-11.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUFIXUPKINDS_H
#define LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUFIXUPKINDS_H

#include "KlaussCPUMCTargetDesc.h"
#include "llvm/MC/MCFixup.h"

namespace llvm {
namespace KlaussCPU {

enum Fixups {
  FK_KlaussCPU_ABS32 = FirstTargetFixupKind,
  FK_KlaussCPU_PCREL32,
  // ISA v3 A1 short branch: simm18 word displacement in word0[17:0],
  // target = PC + 4*disp. Always resolved at assembly time — a short branch
  // whose target is out of range or not in-section is relaxed to the 2-word
  // PC-relative form (KlaussCPUAsmBackend), so it never becomes a relocation.
  FK_KlaussCPU_PCREL18,
  // ISA v3 B fused branch: simm13 word displacement in word0[20:8]. The
  // compiler only fuses when the whole function fits, so it always resolves.
  FK_KlaussCPU_PCREL13,
  // ISA v3 A1 short call, and the short jump of a tail call (`tail`): the
  // field of FK_KlaussCPU_PCREL18.  Resolved at assembly time when the callee
  // is local (static) and in the same section (relaxed to CALLREL / the
  // 2-word `tail` only if it is then out of range); otherwise, a
  // global or weak callee in the same section too, R_KLAUSSCPU_CALL18,
  // which lld resolves, through a thunk when the callee is out of range.
  FK_KlaussCPU_CALL18,
  // hw-proposals 9 LEAPC.S: simm16 byte displacement, high byte in
  // word0[19:12], low byte in word0[7:0], target = PC + disp.  Resolved at
  // assembly time only, like PCREL18: a LEAPC.S whose target is out of range
  // or not in its section is relaxed to the 2-word LEAPC (PCREL32).
  FK_KlaussCPU_PCREL16,
  // TLS local exec: `%tprel(sym)` in the word-1 slot of an ADDI, an indexed
  // load / store or a SETR.  Never resolved at assembly time (the offset from
  // the thread pointer is known only to the linker): R_KLAUSSCPU_TPREL32.
  FK_KlaussCPU_TPREL32,
  // Dynamic linking (ABI.md): the word-1 immediate of a 2-word leapc
  // (`%gotpcrel(sym)`, `%gottprel(sym)`, `%tlsgd(sym)`) or callrel / jmprel /
  // tail (`%plt(sym)`), relative to the instruction like PCREL32.  Never
  // resolved at assembly time: R_KLAUSSCPU_GOTPCREL32, R_KLAUSSCPU_PLT32,
  // R_KLAUSSCPU_TLS_GOTTPREL32, R_KLAUSSCPU_TLS_GD32, against the symbol.
  FK_KlaussCPU_GOTPCREL32,
  FK_KlaussCPU_PLT32,
  FK_KlaussCPU_TLS_GOTTPREL32,
  FK_KlaussCPU_TLS_GD32,
  NumTargetFixupKinds = FK_KlaussCPU_TLS_GD32 + 1 - FirstTargetFixupKind,
};

/// The fixup of a PC-relative specifier (KlaussCPU::isPCRelSpecifier).
inline Fixups getSpecifierFixup(uint16_t S) {
  switch (S) {
  case S_GOTPCREL:
    return FK_KlaussCPU_GOTPCREL32;
  case S_PLT:
    return FK_KlaussCPU_PLT32;
  case S_GOTTPREL:
    return FK_KlaussCPU_TLS_GOTTPREL32;
  default:
    assert(S == S_TLSGD && "not a PC-relative specifier");
    return FK_KlaussCPU_TLS_GD32;
  }
}

} // namespace KlaussCPU
} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUFIXUPKINDS_H
