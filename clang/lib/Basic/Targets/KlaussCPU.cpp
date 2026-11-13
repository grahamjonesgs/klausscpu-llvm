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
  // GCC's spelling of "the __sync builtins work, lock-free, for N bytes",
  // which code such as glib checks before it uses __atomic / __sync
  // builtins: atomics up to 8 bytes are lock-free here (MaxAtomicInlineWidth,
  // __CLANG_ATOMIC_*_LOCK_FREE 2).  Their loads and stores are plain
  // accesses and their RMW / cmpxchg __sync_*_N calls ("forced atomics"):
  // LR/SC loops in musl for Linux user programs, the runtime's atomic.c on
  // bare metal, every size 1/2/4/8 in both.
  Builder.defineMacro("__GCC_HAVE_SYNC_COMPARE_AND_SWAP_1");
  Builder.defineMacro("__GCC_HAVE_SYNC_COMPARE_AND_SWAP_2");
  Builder.defineMacro("__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4");
  Builder.defineMacro("__GCC_HAVE_SYNC_COMPARE_AND_SWAP_8");
}
