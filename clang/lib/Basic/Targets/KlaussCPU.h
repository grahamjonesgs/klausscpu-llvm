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
    // 64-bit little-endian; GPR width = 64 bits.
    // int is 32-bit (standard C); long, long long, and pointers are 64-bit.
    IntWidth = IntAlign = 32;
    LongWidth = LongAlign = 64;
    LongLongWidth = LongLongAlign = 64;
    PointerWidth = PointerAlign = 64;
    // __int128 is 8-aligned (ABI version 3): nothing on the target, stack,
    // malloc or __BIGGEST_ALIGNMENT__, is aligned beyond 8.  The DataLayout
    // has no i128 entry, so LLVM's default (the largest integer entry,
    // i64:64) gives the same.
    Int128Align = 64;
    SizeType    = UnsignedLong;
    PtrDiffType = SignedLong;
    IntPtrType  = SignedLong;
    IntMaxType  = SignedLong;
    Int64Type   = SignedLong;
    resetDataLayout();
    // __thread / _Thread_local for Linux user programs: static executables,
    // so every access is TLS local exec off TP (KlaussCPUISelLowering).  Bare
    // metal sets no thread pointer.
    TLSSupported = Triple.isOSLinux();
    // No hardware FP.
    HasFloat128 = false;
    // Atomics up to 8 bytes are inline: the backend makes their loads and
    // stores plain accesses and their RMW / cmpxchg __sync_* libcalls
    // ("forced atomics", atomic within one core only).  Wider ones are
    // __atomic_* library calls.
    MaxAtomicPromoteWidth = MaxAtomicInlineWidth = 64;
  }

  void getTargetDefines(const LangOptions &Opts,
                        MacroBuilder &Builder) const override;

  bool hasFeature(StringRef Feature) const override {
    return Feature == "klausscpu";
  }

  // CPU names: "generic" (default, M8 scheduling model ON) and "no-sched"
  // (NoSchedModel — the M8 A/B baseline).  Selected with `-mcpu=no-sched`
  // (the driver passes it as -target-cpu).  Without these overrides the base
  // TargetInfo::setCPU returns false and cc1 rejects any -target-cpu.
  bool isValidCPUName(StringRef Name) const override {
    return Name == "generic" || Name == "no-sched";
  }
  void fillValidCPUList(SmallVectorImpl<StringRef> &Values) const override {
    static const char *const CPUs[] = {"generic", "no-sched"};
    Values.append(std::begin(CPUs), std::end(CPUs));
  }
  bool setCPU(const std::string &Name) override { return isValidCPUName(Name); }

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

  // Every asm statement may set the condition flags (most ALU ops do), so
  // treat FLAGS as clobbered by all of them, as x86 does.
  std::string_view getClobbers() const override { return "~{flags}"; }
};

} // namespace targets
} // namespace clang

#endif // LLVM_CLANG_LIB_BASIC_TARGETS_KLAUSSCPU_H
