#include "llvm/MC/TargetRegistry.h"
using namespace llvm;

namespace llvm {
Target &getTheKlaussCPUTarget() {
  static Target TheKlaussCPUTarget;
  return TheKlaussCPUTarget;
}
} // namespace llvm

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeKlaussCPUTargetInfo() {
  RegisterTarget<> X(getTheKlaussCPUTarget(), "klausscpu",
                     "KlaussCPU 64-bit FPGA CPU", "KlaussCPU");
}
