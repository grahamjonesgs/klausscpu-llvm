//===--- KlaussCPU.h - KlaussCPU ToolChain ---------------------*- C++ -*-===//
//
// Clang driver toolchain for KlaussCPU — a bare-metal FPGA target, and
// user programs and shared libraries for its Linux port
// (klausscpu-*-linux-*: static by default, -shared / -pie for musl's dynamic
// linker; ABI.md "Driver").
//
// Selecting this toolchain (via -target klausscpu-unknown-elf) automatically:
//   - restricts includes to Clang's own built-in headers only (no system libc)
//   - leaves the C environment hosted (pass -ffreestanding for code that
//     implements the C library itself, e.g. picolibc, compiler-rt, kernels)
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

  // klausscpu-*-linux-*: user programs and shared libraries against a libc
  // sysroot (musl); otherwise bare metal with clang's headers only.
  bool isLinux() const { return getTriple().isOSLinux(); }
  // The sysroot's root: --sysroot, else <prefix>/klausscpu-linux-musl
  // beside the compiler's bin directory.
  std::string computeSysRoot() const override;

  bool isBareMetal()         const override { return !isLinux(); }
  bool isCrossCompiling()    const override { return true; }
  bool HasNativeLLVMSupport()const override { return true; }
  bool isPICDefault()        const override { return false; }
  bool isPICDefaultForced()  const override { return false; }
  bool SupportsProfiling()   const override { return false; }

  bool isPIEDefault(const llvm::opt::ArgList &) const override {
    return false;
  }
  // The linker is always lld (klausscpu::Linker), so -flto needs no plugin.
  const char *getDefaultLinker() const override { return "ld.lld"; }

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

  // C++ on Linux: libc++, libc++abi and libunwind from the sysroot
  // (<sysroot>/include/c++/v1, <sysroot>/lib); the runtime's
  // linux/cxx/build.sh installs them.
  CXXStdlibType GetDefaultCXXStdlibType() const override {
    return isLinux() ? ToolChain::CST_Libcxx : Generic_ELF::GetDefaultCXXStdlibType();
  }
  void AddClangCXXStdlibIncludeArgs(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &CC1Args) const override;

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
