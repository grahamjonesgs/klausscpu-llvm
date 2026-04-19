//===--- KlaussCPU.cpp - Implement KlaussCPU target feature support -------===//
//
// KlaussCPU LLVM backend — Clang TargetInfo implementation.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPU.h"
#include "clang/Basic/MacroBuilder.h"

using namespace clang;
using namespace clang::targets;

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
