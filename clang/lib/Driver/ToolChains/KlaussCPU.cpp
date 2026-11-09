//===--- KlaussCPU.cpp - KlaussCPU ToolChain ------------------------------===//
//
// Clang driver toolchain for KlaussCPU — a bare-metal FPGA target.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPU.h"
#include "clang/Driver/CommonArgs.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/InputInfo.h"
#include "clang/Options/Options.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

//===----------------------------------------------------------------------===//
// KlaussCPUToolChain
//===----------------------------------------------------------------------===//

KlaussCPUToolChain::KlaussCPUToolChain(const Driver &D,
                                         const llvm::Triple &Triple,
                                         const ArgList &Args)
    : Generic_ELF(D, Triple, Args) {
  // Use lld from the same directory as the compiler.
  getProgramPaths().push_back(D.Dir);
  if (isLinux()) {
    llvm::SmallString<128> Lib(computeSysRoot());
    llvm::sys::path::append(Lib, "lib");
    getFilePaths().push_back(std::string(Lib));
  }
}

std::string KlaussCPUToolChain::computeSysRoot() const {
  const Driver &D = getDriver();
  if (!D.SysRoot.empty())
    return D.SysRoot;
  llvm::SmallString<128> P(D.Dir);
  llvm::sys::path::append(P, "..", "klausscpu-linux-musl");
  return std::string(P);
}

void KlaussCPUToolChain::AddClangSystemIncludeArgs(const ArgList &DriverArgs,
                                                    ArgStringList &CC1Args) const {
  // Honour explicit suppression flags.
  if (DriverArgs.hasArg(options::OPT_nostdinc))
    return;

  // Add only Clang's own built-in headers (stdint.h, stdbool.h, etc.).
  // These are target-independent and don't pull in any libc symbols.
  if (!DriverArgs.hasArg(options::OPT_nobuiltininc)) {
    llvm::SmallString<128> P(getDriver().ResourceDir);
    llvm::sys::path::append(P, "include");
    addSystemInclude(DriverArgs, CC1Args, P);
  }
  // Linux: the libc headers of the sysroot.  Never /usr/include or other
  // host paths.
  if (isLinux() && !DriverArgs.hasArg(options::OPT_nostdlibinc)) {
    llvm::SmallString<128> P(computeSysRoot());
    llvm::sys::path::append(P, "include");
    addExternCSystemInclude(DriverArgs, CC1Args, P);
  }
}

Tool *KlaussCPUToolChain::buildLinker() const {
  return new tools::klausscpu::Linker(*this);
}

//===----------------------------------------------------------------------===//
// klausscpu::Linker
//===----------------------------------------------------------------------===//

void klausscpu::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                                      const InputInfo &Output,
                                      const InputInfoList &Inputs,
                                      const ArgList &Args,
                                      const char *LinkingOutput) const {
  const ToolChain &TC = getToolChain();
  ArgStringList CmdArgs;

  CmdArgs.push_back("-m");
  CmdArgs.push_back("elf32klausscpu");

  CmdArgs.push_back("-o");
  CmdArgs.push_back(Output.getFilename());

  // Explicit -T linker scripts (not wrapped in -Wl,-T).
  Args.AddAllArgs(CmdArgs, options::OPT_T);
  // -L search paths.
  Args.AddAllArgs(CmdArgs, options::OPT_L);

  // Linux user programs: static (there is no dynamic linker), 64 KiB pages
  // (the TLB's only size), the sysroot's crt1.o first.
  const auto &KTC = static_cast<const toolchains::KlaussCPUToolChain &>(TC);
  bool Linux = KTC.isLinux();
  bool StartFiles = !Args.hasArg(options::OPT_nostdlib, options::OPT_nostartfiles);
  bool DefaultLibs = !Args.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs);
  if (Linux) {
    CmdArgs.push_back("-static");
    CmdArgs.push_back("-z");
    CmdArgs.push_back("max-page-size=0x10000");
    for (const auto &Path : TC.getFilePaths())
      CmdArgs.push_back(Args.MakeArgString("-L" + Path));
    if (StartFiles)
      CmdArgs.push_back(Args.MakeArgString(TC.GetFilePath("crt1.o")));
  }

  // -flto: lld optimises the bitcode at the link (-plugin-opt=O<n> from the
  // -O level, as BareMetal; -Os/-Oz are function attributes in the bitcode).
  const Driver &D = TC.getDriver();
  if (D.isUsingLTO())
    addLTOOptions(TC, Args, CmdArgs, Output, Inputs,
                  D.getLTOMode() == LTOK_Thin);

  // Input object files and libraries (including any positional -Wl,-T script).
  AddLinkerInputs(TC, Inputs, Args, CmdArgs, JA);

  // Pass -Wl,X and -Xlinker X flags to lld with the prefix stripped.
  // We iterate manually so we can skip -T: the linker script is already in
  // the Inputs list as a positional file and lld will parse it directly.
  for (const auto *A : Args.filtered(options::OPT_Wl_COMMA,
                                     options::OPT_Xlinker)) {
    A->claim();
    for (StringRef Val : A->getValues()) {
      // Skip "-T" — the script file is added as an input by AddLinkerInputs.
      if (Val == "-T")
        continue;
      CmdArgs.push_back(Val.data());
    }
  }

  // Linux: libc, then compiler-rt's builtins (soft float, 128-bit maths).
  // The builtins are the bare-metal ones when none were built for the
  // Linux triple: they are freestanding and the ABI is the same.
  if (Linux && DefaultLibs) {
    CmdArgs.push_back("--start-group");
    CmdArgs.push_back("-lc");
    std::string RT = TC.getCompilerRT(Args, "builtins");
    if (!TC.getVFS().exists(RT)) {
      llvm::SmallString<128> P(TC.getDriver().ResourceDir);
      llvm::sys::path::append(P, "lib", "klausscpu-unknown-unknown-elf",
                              "libclang_rt.builtins.a");
      RT = std::string(P);
    }
    CmdArgs.push_back(Args.MakeArgString(RT));
    CmdArgs.push_back("--end-group");
  }

  // Pass-through flags that lld understands directly.
  if (Args.hasArg(options::OPT_nostdlib))
    CmdArgs.push_back("-nostdlib");
  if (!Linux && Args.hasArg(options::OPT_static))
    CmdArgs.push_back("-static");

  const char *Exec = Args.MakeArgString(TC.GetProgramPath("ld.lld"));
  C.addCommand(std::make_unique<Command>(JA, *this,
                                          ResponseFileSupport::AtFileCurCP(),
                                          Exec, CmdArgs, Inputs, Output));
}

