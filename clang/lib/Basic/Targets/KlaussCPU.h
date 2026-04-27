//===--- KlaussCPU.h - Declare KlaussCPU target feature support -*- C++ -*-===//
//
// KlaussCPU LLVM backend — Clang TargetInfo declaration.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_BASIC_TARGETS_KLAUSSCPU_H
#define LLVM_CLANG_LIB_BASIC_TARGETS_KLAUSSCPU_H

#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "llvm/Support/Compiler.h"
#include "llvm/TargetParser/Triple.h"

namespace clang {
namespace targets {

class LLVM_LIBRARY_VISIBILITY KlaussCPUTargetInfo : public TargetInfo {
public:
  KlaussCPUTargetInfo(const llvm::Triple &Triple, const TargetOptions &)
      : TargetInfo(Triple) {
    // 64-bit little-endian; all primitive types are 64-bit-aligned.
    // int is standard 32-bit; use long for 64-bit quantities.
    LongWidth = LongAlign = 64;
    LongLongWidth = LongLongAlign = 64;
    PointerWidth = PointerAlign = 64;
    SizeType    = UnsignedLong;
    PtrDiffType = SignedLong;
    IntPtrType  = SignedLong;
    IntMaxType  = SignedLong;
    Int64Type   = SignedLong;
    resetDataLayout("e-m:e-p:64:64-i64:64-i128:128-n64");
    TLSSupported = false;
    // No hardware FP.
    HasFloat128 = false;
  }

  void getTargetDefines(const LangOptions &Opts,
                        MacroBuilder &Builder) const override;

  bool hasFeature(StringRef Feature) const override {
    return Feature == "klausscpu";
  }

  ArrayRef<const char *> getGCCRegNames() const override;

  ArrayRef<TargetInfo::GCCRegAlias> getGCCRegAliases() const override {
    return {};
  }

  BuiltinVaListKind getBuiltinVaListKind() const override {
    return TargetInfo::VoidPtrBuiltinVaList;
  }

  llvm::SmallVector<Builtin::InfosShard> getTargetBuiltins() const override;

  bool validateAsmConstraint(const char *&Name,
                             TargetInfo::ConstraintInfo &Info) const override {
    return false;
  }

  std::string_view getClobbers() const override { return ""; }
};

} // namespace targets
} // namespace clang

#endif // LLVM_CLANG_LIB_BASIC_TARGETS_KLAUSSCPU_H
