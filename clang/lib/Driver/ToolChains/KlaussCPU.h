//===--- KlaussCPU.h - KlaussCPU ToolChain ---------------------*- C++ -*-===//
//
// Clang driver toolchain for KlaussCPU — a bare-metal FPGA target.
//
// Selecting this toolchain (via -target klausscpu-unknown-elf) automatically:
//   - restricts includes to Clang's own built-in headers only (no system libc)
//   - enables -ffreestanding so the compiler makes no libc assumptions
//   - uses lld as the linker with no default start files or standard libs
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_KLAUSSCPU_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_KLAUSSCPU_H

#include "Gnu.h"
#include "clang/Driver/Tool.h"
#include "clang/Driver/ToolChain.h"

namespace clang {
namespace driver {
namespace toolchains {

class LLVM_LIBRARY_VISIBILITY KlaussCPUToolChain : public Generic_ELF {
public:
  KlaussCPUToolChain(const Driver &D, const llvm::Triple &Triple,
                      const llvm::opt::ArgList &Args);

  bool isBareMetal()         const override { return true; }
  bool isCrossCompiling()    const override { return true; }
  bool HasNativeLLVMSupport()const override { return true; }
  bool isPICDefault()        const override { return false; }
  bool isPICDefaultForced()  const override { return false; }
  bool SupportsProfiling()   const override { return false; }

  bool isPIEDefault(const llvm::opt::ArgList &) const override {
    return false;
  }

  RuntimeLibType GetDefaultRuntimeLibType() const override {
    return RuntimeLibType::RLT_CompilerRT;
  }
  UnwindLibType GetUnwindLibType(const llvm::opt::ArgList &Args) const override {
    return UnwindLibType::UNW_None;
  }
  UnwindTableLevel
  getDefaultUnwindTableLevel(const llvm::opt::ArgList &) const override {
    return UnwindTableLevel::None;
  }

  // Only Clang's own built-in headers; never /usr/include.
  void AddClangSystemIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                                  llvm::opt::ArgStringList &CC1Args) const override;

  // Inject -ffreestanding so the compiler makes no libc assumptions.
  void addClangTargetOptions(const llvm::opt::ArgList &DriverArgs,
                              llvm::opt::ArgStringList &CC1Args,
                              Action::OffloadKind) const override;

protected:
  Tool *buildLinker() const override;
};

} // namespace toolchains

namespace tools {
namespace klausscpu {

// Bare-metal lld linker for KlaussCPU.
class LLVM_LIBRARY_VISIBILITY Linker final : public Tool {
public:
  Linker(const ToolChain &TC) : Tool("klausscpu::Linker", "linker", TC) {}
  bool isLinkJob()        const override { return true; }
  bool hasIntegratedCPP() const override { return false; }
  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const llvm::opt::ArgList &TCArgs,
                    const char *LinkingOutput) const override;
};

} // namespace klausscpu
} // namespace tools

} // namespace driver
} // namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_KLAUSSCPU_H
