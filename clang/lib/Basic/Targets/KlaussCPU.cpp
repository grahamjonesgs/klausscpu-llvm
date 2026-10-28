//===--- KlaussCPU.cpp - Implement KlaussCPU target feature support -------===//
//
// KlaussCPU LLVM backend — Clang TargetInfo implementation.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPU.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/MacroBuilder.h"
#include "llvm/TargetParser/KlaussCPUABI.h"

using namespace clang;
using namespace clang::targets;

llvm::SmallVector<Builtin::InfosShard>
KlaussCPUTargetInfo::getTargetBuiltins() const {
  // UART is memory-mapped (MMIO); KlaussCPU defines no target builtins.
  return {};
}

// R0–R15 (16 GPRs).
ArrayRef<const char *> KlaussCPUTargetInfo::getGCCRegNames() const {
  return llvm::ArrayRef(llvm::KlaussCPUABI::GPRNames);
}

void KlaussCPUTargetInfo::getTargetDefines(const LangOptions &Opts,
                                           MacroBuilder &Builder) const {
  Builder.defineMacro("__klausscpu__");
  Builder.defineMacro("__KlaussCPU__");
  // The calling convention (KlaussCPUABI.h), for code that depends on it,
  // such as hand-written assembly.
  Builder.defineMacro("__klausscpu_abi__",
                      llvm::Twine(llvm::KlaussCPUABI::ABIVersion));
}
