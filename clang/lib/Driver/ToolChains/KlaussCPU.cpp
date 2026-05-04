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
#include "llvm/Support/FileSystem.h"
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
  // Bare-metal target: no OS, no libc.  The compiler must not assume any
  // library functions are available.
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

  // Select ELF 32-bit little-endian KlaussCPU emulation.
  CmdArgs.push_back("-m");
  CmdArgs.push_back("elf32klausscpu");

  // Output file.
  CmdArgs.push_back("-o");
  CmdArgs.push_back(Output.getFilename());

  // Linker script: honour -T if the user provided one.
  Args.AddAllArgs(CmdArgs, options::OPT_T);

  // Pass through -L search paths.
  Args.AddAllArgs(CmdArgs, options::OPT_L);

  // Pass through -rpath.
  Args.AddAllArgs(CmdArgs, options::OPT_rpath);

  // No default start files or standard libraries unless the user asks for
  // them explicitly by NOT passing -nostartfiles / -nodefaultlibs.
  // KlaussCPU programs must link their own crt0.o and libc.o.
  if (Args.hasArg(options::OPT_nostdlib) ||
      Args.hasArg(options::OPT_nodefaultlibs)) {
    // User explicitly said no default libs — honour it.
  }
  // Always suppress the host's standard startup files and libraries; they are
  // not meaningful for a bare-metal target.
  CmdArgs.push_back("-nostdlib");

  // Add all input object files and libraries.
  AddLinkerInputs(TC, Inputs, Args, CmdArgs, JA);

  // Pass through any extra -Wl,... flags the user supplied.
  Args.AddAllArgs(CmdArgs, options::OPT_Wl_COMMA);

  // Find ld.lld next to the compiler.
  const char *Exec = Args.MakeArgString(TC.GetProgramPath("ld.lld"));
  C.addCommand(std::make_unique<Command>(JA, *this,
                                          ResponseFileSupport::AtFileCurCP(),
                                          Exec, CmdArgs, Inputs, Output));
}
