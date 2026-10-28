//===-- KlaussCPUTargetMachine.cpp - KlaussCPU Target Machine -------------===//
//
// KlaussCPU LLVM backend — target machine implementation.
//
// DataLayout (Triple::computeDataLayout, TargetDataLayout.cpp):
//   e-m:e-p:64:64-i64:64-i128:128-n32:64
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
#include "llvm/CodeGen/MacroFusion.h"
#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Target/TargetOptions.h"
#include <optional>

namespace llvm {
FunctionPass *createKlaussCPUISelDag(KlaussCPUTargetMachine &TM);
FunctionPass *createKlaussCPUCalleeSavedCFIPass();
FunctionPass *createKlaussCPUConstAddrBasePass();
FunctionPass *createKlaussCPUEarlyIVIncPass();
FunctionPass *createKlaussCPUFlagReusePass();
FunctionPass *createKlaussCPUFuseCmpBrPass(bool LoopConstsOnly = false);
FunctionPass *createKlaussCPUJumpTableHoistPass();
FunctionPass *createKlaussCPULeaveRetPass();
FunctionPass *createKlaussCPUOptExtensionsPass();
void initializeKlaussCPUCalleeSavedCFIPass(PassRegistry &);
void initializeKlaussCPUConstAddrBasePass(PassRegistry &);
void initializeKlaussCPUDAGToDAGISelLegacyPass(PassRegistry &);
void initializeKlaussCPUEarlyIVIncPass(PassRegistry &);
void initializeKlaussCPUFlagReusePass(PassRegistry &);
void initializeKlaussCPUFuseCmpBrPass(PassRegistry &);
void initializeKlaussCPUJumpTableHoistPass(PassRegistry &);
void initializeKlaussCPULeaveRetPass(PassRegistry &);
void initializeKlaussCPUOptExtensionsPass(PassRegistry &);
} // namespace llvm

using namespace llvm;

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeKlaussCPUTarget() {
  RegisterTargetMachine<KlaussCPUTargetMachine> X(getTheKlaussCPUTarget());
  // Register the target's MachineFunction passes so MIR tests can run them
  // with -run-pass.
  PassRegistry &PR = *PassRegistry::getPassRegistry();
  initializeKlaussCPUCalleeSavedCFIPass(PR);
  initializeKlaussCPUConstAddrBasePass(PR);
  initializeKlaussCPUDAGToDAGISelLegacyPass(PR);
  initializeKlaussCPUEarlyIVIncPass(PR);
  initializeKlaussCPUFlagReusePass(PR);
  initializeKlaussCPUFuseCmpBrPass(PR);
  initializeKlaussCPUJumpTableHoistPass(PR);
  initializeKlaussCPULeaveRetPass(PR);
  initializeKlaussCPUOptExtensionsPass(PR);
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
    : CodeGenTargetMachineImpl(T, TT.computeDataLayout(), TT,
                                getEffectiveCPU(CPU), FS, Options,
                                getEffectiveRelocModel(RM),
                                getEffectiveCodeModel(CM, CodeModel::Small), OL),
      Subtarget(TT, getEffectiveCPU(CPU), FS, *this),
      TLOF(std::make_unique<TargetLoweringObjectFileELF>()) {
  initAsmInfo();
  // Epilogues that end in a plain LEAVE tear the frame down before the block's
  // terminator; CFIFixup re-establishes the frame CFI for later blocks.
  setCFIFixup(true);
}

// The pre-RA scheduler keeps a compare next to the conditional branch that
// reads its flags, the pair KlaussCPUFuseCmpBr fuses after register
// allocation.  An instruction the scheduler puts in between may get a
// compare operand's register (it is free once the compare has read it), and
// that redefinition blocks the fusion.
static cl::opt<bool> SchedFuseCmpBr(
    "klausscpu-sched-cmp-br-pair", cl::Hidden, cl::init(true),
    cl::desc("Pre-RA scheduler: keep a compare adjacent to its branch"));

static bool isCmpBrPair(const TargetInstrInfo &, const TargetSubtargetInfo &,
                        const MachineInstr *First, const MachineInstr &Second) {
  if (!Second.isConditionalBranch() ||
      !Second.readsRegister(KlaussCPU::FLAGS, /*TRI=*/nullptr))
    return false;
  if (!First)
    return true;
  switch (First->getOpcode()) {
  case KlaussCPU::CMPRR_I:
  case KlaussCPU::CMPRV_I:
  case KlaussCPU::CMPRVU_I:
    return true;
  default:
    return false;
  }
}

ScheduleDAGInstrs *
KlaussCPUTargetMachine::createMachineScheduler(MachineSchedContext *C) const {
  ScheduleDAGMILive *DAG = createSchedLive(C);
  if (SchedFuseCmpBr)
    DAG->addMutation(createMacroFusionDAGMutation({isCmpBrPair}));
  return DAG;
}

//===----------------------------------------------------------------------===//
// Pass configuration
//===----------------------------------------------------------------------===//

static cl::opt<cl::boolOrDefault>
    EnableGlobalMerge("klausscpu-enable-global-merge", cl::Hidden,
                      cl::desc("Enable the global merge pass"));

namespace {
class KlaussCPUPassConfig : public TargetPassConfig {
public:
  KlaussCPUPassConfig(KlaussCPUTargetMachine &TM, PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {}

  KlaussCPUTargetMachine &getKlaussCPUTargetMachine() const {
    return getTM<KlaussCPUTargetMachine>();
  }

  void addIRPasses() override;
  bool addPreISel() override;
  bool addInstSelector() override;
  void addMachineSSAOptimization() override;
  bool addILPOpts() override;
  void addPreRegAlloc() override;
  void addPreSched2() override;
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

void KlaussCPUPassConfig::addIRPasses() {
  // Forced atomics (KlaussCPUTargetLowering constructor): atomics wider than
  // 64 bits or misaligned become __atomic_* libcalls, the RMWs without a
  // __sync_* libcall (FP, uinc/udec_wrap, usub_cond/sat) cmpxchg loops.
  addPass(createAtomicExpandLegacyPass());
  TargetPassConfig::addIRPasses();
  // After loop strength reduction has settled each loop's counter.
  if (getOptLevel() != CodeGenOptLevel::None)
    addPass(createKlaussCPUEarlyIVIncPass());
}

bool KlaussCPUPassConfig::addPreISel() {
  if ((getOptLevel() != CodeGenOptLevel::None &&
       EnableGlobalMerge == cl::BOU_UNSET) ||
      EnableGlobalMerge == cl::BOU_TRUE)
    addPass(createGlobalMergePass(TM, /*MaxOffset=*/1016,
                                  /*OnlyOptimizeForSize=*/false,
                                  /*MergeExternalByDefault=*/false));
  return false;
}

bool KlaussCPUPassConfig::addInstSelector() {
  addPass(createKlaussCPUISelDag(getKlaussCPUTargetMachine()));
  return false;
}

void KlaussCPUPassConfig::addMachineSSAOptimization() {
  TargetPassConfig::addMachineSSAOptimization();
  // Extensions made redundant across blocks (KlaussCPUOptExtensions.cpp).
  addPass(createKlaussCPUOptExtensionsPass());
}

bool KlaussCPUPassConfig::addILPOpts() {
  // One base register for constant-address accesses near each other,
  // before MachineLICM takes it out of loops (KlaussCPUConstAddrBase.cpp).
  addPass(createKlaussCPUConstAddrBasePass());
  return true;
}

void KlaussCPUPassConfig::addPreRegAlloc() {
  // A jump table's base and entry offset into the switch's range-check
  // block, after MachineSink (KlaussCPUJumpTableHoist.cpp).
  if (getOptLevel() != CodeGenOptLevel::None)
    addPass(createKlaussCPUJumpTableHoistPass());
}

void KlaussCPUPassConfig::addPreSched2() {
  // The FuseCmpBr loop constants, before the post-RA scheduler's register
  // renaming can take the free register they need (KlaussCPUFuseCmpBr.cpp).
  if (getOptLevel() != CodeGenOptLevel::None)
    addPass(createKlaussCPUFuseCmpBrPass(/*LoopConstsOnly=*/true));
}

void KlaussCPUPassConfig::addPreEmitPass() {
  // A shrink-wrapped epilogue's LEAVE + RET (after tail duplication and
  // block placement) becomes LEAVERET (KlaussCPULeaveRet.cpp).
  if (getOptLevel() != CodeGenOptLevel::None)
    addPass(createKlaussCPULeaveRetPass());
  // A3a — arithmetic Z-flag reuse.  Runs on the final (post-RA, post-
  // scheduling) instruction stream so adjacency is meaningful.  On by default;
  // -klausscpu-arith-flag-reuse=false disables it.
  addPass(createKlaussCPUFlagReusePass());
  // ISA v3 B — fuse the remaining adjacent compare + conditional branch.
  addPass(createKlaussCPUFuseCmpBrPass());
  // The callee-saved .cfi_offset rules, behind the scheduled spills (with -g
  // or unwind tables only; before CFIFixup, which copies them into the
  // state it restores).
  addPass(createKlaussCPUCalleeSavedCFIPass());
}

TargetTransformInfo
KlaussCPUTargetMachine::getTargetTransformInfo(const Function &F) const {
  return TargetTransformInfo(std::make_unique<KlaussCPUTTIImpl>(this, F));
}
