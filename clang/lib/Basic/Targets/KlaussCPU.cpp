//===--- KlaussCPU.cpp - Implement KlaussCPU target feature support -------===//
//
// KlaussCPU LLVM backend — Clang TargetInfo implementation.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPU.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/MacroBuilder.h"
#include "clang/Basic/TargetBuiltins.h"

using namespace clang;
using namespace clang::targets;

static constexpr int NumKlaussCPUBuiltins =
    KlaussCPU::LastTSBuiltin - Builtin::FirstTSBuiltin;

static constexpr llvm::StringTable KlaussCPUBuiltinStrings =
    CLANG_BUILTIN_STR_TABLE_START
#define BUILTIN CLANG_BUILTIN_STR_TABLE
#include "clang/Basic/BuiltinsKlaussCPU.def"
    ;

static constexpr auto KlaussCPUBuiltinInfos =
    Builtin::MakeInfos<NumKlaussCPUBuiltins>({
#define BUILTIN CLANG_BUILTIN_ENTRY
#include "clang/Basic/BuiltinsKlaussCPU.def"
    });

llvm::SmallVector<Builtin::InfosShard>
KlaussCPUTargetInfo::getTargetBuiltins() const {
  return {{&KlaussCPUBuiltinStrings, KlaussCPUBuiltinInfos}};
}

// R0–R15 (16 GPRs).
static const char *const GCCRegNames[] = {
    "r0", "r1", "r2",  "r3",  "r4",  "r5",  "r6",  "r7",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
};

ArrayRef<const char *> KlaussCPUTargetInfo::getGCCRegNames() const {
  return llvm::ArrayRef(GCCRegNames);
}

void KlaussCPUTargetInfo::getTargetDefines(const LangOptions &Opts,
                                           MacroBuilder &Builder) const {
  Builder.defineMacro("__klausscpu__");
  Builder.defineMacro("__KlaussCPU__");
}
