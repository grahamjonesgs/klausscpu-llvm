//===-- KlaussCPUTargetMachine.h - KlaussCPU TargetMachine ------*- C++ -*-===//
//
// KlaussCPU LLVM backend — target machine class declaration.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUTARGETMACHINE_H
#define LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUTARGETMACHINE_H

#include "KlaussCPUSubtarget.h"
#include "llvm/CodeGen/CodeGenTargetMachineImpl.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/Support/Allocator.h"
#include <optional>

namespace llvm {

class KlaussCPUTargetMachine : public CodeGenTargetMachineImpl {
  KlaussCPUSubtarget Subtarget;
  std::unique_ptr<TargetLoweringObjectFile> TLOF;

public:
  KlaussCPUTargetMachine(const Target &T, const Triple &TT, StringRef CPU,
                          StringRef FS, const TargetOptions &Options,
                          std::optional<Reloc::Model> RM,
                          std::optional<CodeModel::Model> CM,
                          CodeGenOptLevel OL, bool JIT);

  const KlaussCPUSubtarget *
  getSubtargetImpl(const Function & /*F*/) const override {
    return &Subtarget;
  }

  TargetPassConfig *createPassConfig(PassManagerBase &PM) override;

  MachineFunctionInfo *
  createMachineFunctionInfo(BumpPtrAllocator &Allocator, const Function &F,
                             const TargetSubtargetInfo *STI) const override;

  TargetLoweringObjectFile *getObjFileLowering() const override {
    return TLOF.get();
  }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUTARGETMACHINE_H
