//===-- KlaussCPUFixupKinds.h - KlaussCPU fixup entry definitions ----------===//
//
// One fixup kind: FK_KlaussCPU_ABS32
//   32-bit absolute address placed in word 1 (bytes [4:7]) of an 8-byte
//   branch or call instruction.  Written big-endian (MSB first).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUFIXUPKINDS_H
#define LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUFIXUPKINDS_H

#include "llvm/MC/MCFixup.h"

namespace llvm {
namespace KlaussCPU {

enum Fixups {
  FK_KlaussCPU_ABS32 = FirstTargetFixupKind,
  NumTargetFixupKinds = FK_KlaussCPU_ABS32 + 1 - FirstTargetFixupKind,
};

} // namespace KlaussCPU
} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUFIXUPKINDS_H
