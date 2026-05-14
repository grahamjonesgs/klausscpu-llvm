//===-- KlaussCPUFixupKinds.h - KlaussCPU fixup entry definitions ----------===//
//
// FK_KlaussCPU_ABS32   — 32-bit absolute address, word 1 (bytes [4:7]) of an
//                         8-byte branch/call/SETR instruction. LE byte order.
// FK_KlaussCPU_PCREL32 — 32-bit signed PC-relative offset, word 1 of an 8-byte
//                         JMPREL/JMPxxREL/CALLREL/LEAPC instruction.
//                         field_value = target - PC_of_instruction.
//                         (PC_of_instruction = instr_addr, NOT instr_addr+4.)
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
  NumTargetFixupKinds = FK_KlaussCPU_PCREL32 + 1 - FirstTargetFixupKind,
};

} // namespace KlaussCPU
} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUFIXUPKINDS_H
