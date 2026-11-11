//===--- KlaussCPU.cpp - KlaussCPU ToolChain ------------------------------===//
//
// Clang driver toolchain for KlaussCPU — a bare-metal FPGA target, and
// user programs and shared libraries for its Linux port.
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

  // Linux user programs (ABI.md "Dynamic linking", "Driver"): static by
  // default, with the sysroot's crt1.o first; -shared links a shared library
  // (no start file), -pie a PIE (Scrt1.o) for musl's dynamic linker.  64 KiB
  // pages (the TLB's only size) for all three.  -static wins over -pie.
  const auto &KTC = static_cast<const toolchains::KlaussCPUToolChain &>(TC);
  bool Linux = KTC.isLinux();
  bool StartFiles = !Args.hasArg(options::OPT_nostdlib, options::OPT_nostartfiles);
  bool DefaultLibs = !Args.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs);
  bool Shared = Linux && Args.hasArg(options::OPT_shared);
  bool WantPIE = Args.hasFlag(options::OPT_pie, options::OPT_no_pie, false);
  bool PIE = Linux && !Shared && WantPIE && !Args.hasArg(options::OPT_static);
  // -rdynamic: export every symbol; a static executable has no .dynsym.
  bool RDynamic = Linux && Args.hasArg(options::OPT_rdynamic);
  if (Linux) {
    if (Shared) {
      CmdArgs.push_back("-shared");
    } else if (PIE) {
      CmdArgs.push_back("-pie");
      CmdArgs.push_back("-dynamic-linker");
      CmdArgs.push_back("/lib/ld-musl-klausscpu.so.1");
    } else {
      CmdArgs.push_back("-static");
    }
    if (Shared || PIE) {
      // No lazy binding (musl binds every PLT slot at load); .got.plt in
      // PT_GNU_RELRO.  lld defaults to the same for KlaussCPU.
      CmdArgs.push_back("-z");
      CmdArgs.push_back("now");
      CmdArgs.push_back("--hash-style=both");
      CmdArgs.push_back("--eh-frame-hdr");
      if (RDynamic)
        CmdArgs.push_back("--export-dynamic");
    }
    CmdArgs.push_back("-z");
    CmdArgs.push_back("max-page-size=0x10000");
    if (Shared || PIE) {
      CmdArgs.push_back("-z");
      CmdArgs.push_back("common-page-size=0x10000");
    }
    for (const auto &Path : TC.getFilePaths())
      CmdArgs.push_back(Args.MakeArgString("-L" + Path));
    if (StartFiles && !Shared)
      CmdArgs.push_back(
          Args.MakeArgString(TC.GetFilePath(PIE ? "Scrt1.o" : "crt1.o")));
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
  // A static link takes the bare-metal builtins when none were built for the
  // Linux triple: they are freestanding and the ABI is the same.  A shared
  // library or PIE needs the Linux ones (-fPIC, hidden; ABI.md "Building
  // shared libraries"): the bare-metal archive's absolute jump tables cannot
  // be linked into one.
  if (Linux && DefaultLibs) {
    CmdArgs.push_back("--start-group");
    CmdArgs.push_back("-lc");
    std::string RT = TC.getCompilerRT(Args, "builtins");
    if (!Shared && !PIE && !TC.getVFS().exists(RT)) {
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

