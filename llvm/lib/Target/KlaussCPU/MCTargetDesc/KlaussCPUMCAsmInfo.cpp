//===-- KlaussCPUMCAsmInfo.cpp - KlaussCPU asm properties -----------------===//
//
// KlaussCPU LLVM backend — MCAsmInfo implementation.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUMCAsmInfo.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

void KlaussCPUMCAsmInfo::anchor() {}

KlaussCPUMCAsmInfo::KlaussCPUMCAsmInfo(const Triple & /*TheTriple*/,
                                        const MCTargetOptions & /*Options*/) {
  IsLittleEndian = true;             // register file is little-endian (DataLayout 'e')
  CommentString = "#";
  SupportsDebugInformation = true;
  ExceptionsType = ExceptionHandling::DwarfCFI;
  MinInstAlignment = 4;              // all instructions are 4 or 8 bytes
}
