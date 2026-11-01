//===--- KlaussCPU.cpp - Implement KlaussCPU target feature support -------===//
//
// KlaussCPU LLVM backend — Clang TargetInfo implementation.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPU.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/MacroBuilder.h"
#include "clang/Basic/TargetBuiltins.h"
#include "llvm/TargetParser/KlaussCPUABI.h"

using namespace clang;
using namespace clang::targets;

static constexpr int NumBuiltins =
    clang::KlaussCPU::LastTSBuiltin - Builtin::FirstTSBuiltin;

#define GET_BUILTIN_STR_TABLE
#include "clang/Basic/BuiltinsKlaussCPU.inc"
#undef GET_BUILTIN_STR_TABLE

static constexpr Builtin::Info BuiltinInfos[] = {
#define GET_BUILTIN_INFOS
#include "clang/Basic/BuiltinsKlaussCPU.inc"
#undef GET_BUILTIN_INFOS
};
static_assert(std::size(BuiltinInfos) == NumBuiltins);

// UART is memory-mapped (MMIO); the only target builtins are DI / EI.
llvm::SmallVector<Builtin::InfosShard>
KlaussCPUTargetInfo::getTargetBuiltins() const {
  return {{&BuiltinStrings, BuiltinInfos}};
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
