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
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUFIXUPKINDS_H
#define LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUFIXUPKINDS_H

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
  // ISA v3 A1 short call: the field of FK_KlaussCPU_PCREL18.  Resolved at
  // assembly time when the callee is local (static) and in the same section
  // (relaxed to CALLREL only if it is then out of range); otherwise, a
  // global or weak callee in the same section too, R_KLAUSSCPU_CALL18,
  // which lld resolves, through a thunk when the callee is out of range.
  FK_KlaussCPU_CALL18,
  NumTargetFixupKinds = FK_KlaussCPU_CALL18 + 1 - FirstTargetFixupKind,
};

} // namespace KlaussCPU
} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUFIXUPKINDS_H
