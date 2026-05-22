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
  // Never add /usr/include or other system paths — there is no host libc
  // for KlaussCPU.
}

void KlaussCPUToolChain::addClangTargetOptions(const ArgList &DriverArgs,
                                                ArgStringList &CC1Args,
                                                Action::OffloadKind) const {
  if (!DriverArgs.hasArg(options::OPT_ffreestanding))
    CC1Args.push_back("-ffreestanding");
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

  // Pass-through flags that lld understands directly.
  if (Args.hasArg(options::OPT_nostdlib))
    CmdArgs.push_back("-nostdlib");
  if (Args.hasArg(options::OPT_static))
    CmdArgs.push_back("-static");

  const char *Exec = Args.MakeArgString(TC.GetProgramPath("ld.lld"));
  C.addCommand(std::make_unique<Command>(JA, *this,
                                          ResponseFileSupport::AtFileCurCP(),
                                          Exec, CmdArgs, Inputs, Output));
}

