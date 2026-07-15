//===-- KlaussCPUTargetMachine.cpp - KlaussCPU Target Machine -------------===//
//
// KlaussCPU LLVM backend — target machine implementation.
//
// DataLayout: e-m:e-p:64:64-i64:64-i128:128-n32:64
//   e         = little-endian (memory bus and register file)
//   m:e       = ELF name mangling
//   p:64:64   = pointers are 64-bit (GPR-width); the hardware only uses bits
//               [31:0] of an address (128 MiB RAM), but LLVM's type legalizer
//               has no built-in handler for promoting ISD::FrameIndex from i32
//               to i64.  Making pointers 64-bit avoids the issue: FrameIndex is
//               typed as i64 (a legal GPR type) from the outset.  The hardware
//               naturally ignores the upper 32 bits of every address.
//   i64:64    = 64-bit integers are 64-bit aligned
//   n32:64    = i32 (C int) and i64 (long, long long, pointer) are advertised
//               as native widths.  i32 has no register class; it is promoted
//               to i64 by the type legalizer for ALU operations.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUTargetMachine.h"
#include "KlaussCPUMachineFunctionInfo.h"
#include "KlaussCPUTargetTransformInfo.h"
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
FunctionPass *createKlaussCPUFlagReusePass();
} // namespace llvm

using namespace llvm;

// KlaussCPU DataLayout string — fixed for this architecture.
static const char KlaussCPUDataLayout[] =
    "e-m:e-p:64:64-i64:64-i128:128-n32:64";

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeKlaussCPUTarget() {
  RegisterTargetMachine<KlaussCPUTargetMachine> X(getTheKlaussCPUTarget());
}

static Reloc::Model getEffectiveRelocModel(std::optional<Reloc::Model> RM) {
  return RM.value_or(Reloc::Static);
}

// Resolve the default CPU to "generic" (the M8 ProcessorModel).  Without this the
// TargetMachine/Subtarget are built with an empty CPU string, getSchedModelForCPU("")
// returns NoSchedModel, and the M8 scheduling model + post-RA scheduler are inert
// unless -mcpu=generic is passed explicitly.  `-mcpu=no-sched` (non-empty) is left
// untouched so it still selects NoSchedModel as the A/B baseline.
static StringRef getEffectiveCPU(StringRef CPU) {
  return CPU.empty() ? StringRef("generic") : CPU;
}

KlaussCPUTargetMachine::KlaussCPUTargetMachine(
    const Target &T, const Triple &TT, StringRef CPU, StringRef FS,
    const TargetOptions &Options, std::optional<Reloc::Model> RM,
    std::optional<CodeModel::Model> CM, CodeGenOptLevel OL, bool JIT)
    : CodeGenTargetMachineImpl(T, KlaussCPUDataLayout, TT, getEffectiveCPU(CPU), FS,
                                Options, getEffectiveRelocModel(RM),
                                getEffectiveCodeModel(CM, CodeModel::Small), OL),
      Subtarget(TT, getEffectiveCPU(CPU), FS, *this),
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
  void addPreEmitPass() override;
};
} // namespace

TargetPassConfig *
KlaussCPUTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new KlaussCPUPassConfig(*this, PM);
}

MachineFunctionInfo *
KlaussCPUTargetMachine::createMachineFunctionInfo(BumpPtrAllocator &Allocator,
                                                   const Function &F,
                                                   const TargetSubtargetInfo *STI) const {
  return KlaussCPUMachineFunctionInfo::create<KlaussCPUMachineFunctionInfo>(
      Allocator, F, STI);
}

bool KlaussCPUPassConfig::addInstSelector() {
  addPass(createKlaussCPUISelDag(getKlaussCPUTargetMachine()));
  return false;
}

void KlaussCPUPassConfig::addPreEmitPass() {
  // A3a — arithmetic zero_flag reuse.  Runs on the final (post-RA, post-
  // scheduling) instruction stream so adjacency is meaningful.  No-op unless
  // -klausscpu-arith-flag-reuse is set.
  addPass(createKlaussCPUFlagReusePass());
}

TargetTransformInfo
KlaussCPUTargetMachine::getTargetTransformInfo(const Function &F) const {
  return TargetTransformInfo(std::make_unique<KlaussCPUTTIImpl>(this, F));
}
