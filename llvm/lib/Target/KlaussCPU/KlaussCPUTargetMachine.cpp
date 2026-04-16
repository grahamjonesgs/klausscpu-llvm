//===-- KlaussCPUTargetMachine.cpp - KlaussCPU Target Machine -------------===//
//
// KlaussCPU LLVM backend — target machine implementation.
//
// DataLayout: e-m:e-p:64:64-i64:64-n64
//   e       = little-endian (memory bus and register file)
//   m:e     = ELF name mangling
//   p:64:64 = pointers are 64-bit (GPR-width); the hardware only uses bits
//             [31:0] of an address (128 MiB RAM), but LLVM's type legalizer
//             has no built-in handler for promoting ISD::FrameIndex from i32
//             to i64.  Making pointers 64-bit avoids the issue: FrameIndex is
//             typed as i64 (a legal GPR type) from the outset.  The hardware
//             naturally ignores the upper 32 bits of every address.
//   i64:64  = 64-bit integers are 64-bit aligned
//   n64     = native integer width is 64 bits (the only GPR class)
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUTargetMachine.h"
#include "TargetInfo/KlaussCPUTargetInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Target/TargetOptions.h"
#include <optional>

namespace llvm {
FunctionPass *createKlaussCPUISelDag(KlaussCPUTargetMachine &TM);
} // namespace llvm

using namespace llvm;

// KlaussCPU DataLayout string — fixed for this architecture.
static const char KlaussCPUDataLayout[] =
    "e-m:e-p:64:64-i64:64-n64";

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeKlaussCPUTarget() {
  RegisterTargetMachine<KlaussCPUTargetMachine> X(getTheKlaussCPUTarget());
}

static Reloc::Model getEffectiveRelocModel(std::optional<Reloc::Model> RM) {
  return RM.value_or(Reloc::Static);
}

KlaussCPUTargetMachine::KlaussCPUTargetMachine(
    const Target &T, const Triple &TT, StringRef CPU, StringRef FS,
    const TargetOptions &Options, std::optional<Reloc::Model> RM,
    std::optional<CodeModel::Model> CM, CodeGenOptLevel OL, bool JIT)
    : CodeGenTargetMachineImpl(T, KlaussCPUDataLayout, TT, CPU, FS, Options,
                                getEffectiveRelocModel(RM),
                                getEffectiveCodeModel(CM, CodeModel::Small), OL),
      Subtarget(TT, CPU, FS, *this),
      TLOF(std::make_unique<TargetLoweringObjectFileELF>()) {
  initAsmInfo();
}

//===----------------------------------------------------------------------===//
// Pass configuration
//===----------------------------------------------------------------------===//

namespace {
class KlaussCPUPassConfig : public TargetPassConfig {
public:
  KlaussCPUPassConfig(KlaussCPUTargetMachine &TM, PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {}

  KlaussCPUTargetMachine &getKlaussCPUTargetMachine() const {
    return getTM<KlaussCPUTargetMachine>();
  }

  bool addInstSelector() override;
};
} // namespace

TargetPassConfig *
KlaussCPUTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new KlaussCPUPassConfig(*this, PM);
}

bool KlaussCPUPassConfig::addInstSelector() {
  addPass(createKlaussCPUISelDag(getKlaussCPUTargetMachine()));
  return false;
}
