//===- KlaussCPU.cpp ------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ABIInfoImpl.h"
#include "TargetInfo.h"
#include "llvm/TargetParser/KlaussCPUABI.h"

using namespace clang;
using namespace clang::CodeGen;

//===----------------------------------------------------------------------===//
// KlaussCPU ABI Implementation
//
// Arguments and returns are classified as DefaultABIInfo does (aggregates
// indirect; the backend's calling convention, KlaussCPUCallingConv.td, does
// the rest).  Only va_arg differs.
//===----------------------------------------------------------------------===//

namespace {

class KlaussCPUABIInfo : public DefaultABIInfo {
public:
  KlaussCPUABIInfo(CodeGenTypes &CGT) : DefaultABIInfo(CGT) {}

  // va_list is a plain pointer to 8-byte slots: every argument is promoted to
  // i64 or split into i64 parts, the stack arguments follow the home area of
  // R1-R3 without gaps, and nothing is aligned beyond 8.  An indirect
  // (aggregate) argument's slot holds its address.  The pointer bump is plain
  // IR, so the optimiser can keep the va_list pointer in a register (the
  // generic va_arg instruction is opaque to it).
  RValue EmitVAArg(CodeGenFunction &CGF, Address VAListAddr, QualType Ty,
                   AggValueSlot Slot) const override {
    ABIArgInfo AI = classifyArgumentType(Ty);
    return emitVoidPtrVAArg(
        CGF, VAListAddr, Ty, AI.isIndirect(),
        getContext().getTypeInfoInChars(Ty),
        CharUnits::fromQuantity(llvm::KlaussCPUABI::SlotSize),
        /*AllowHigherAlign=*/false, Slot);
  }
};

class KlaussCPUTargetCodeGenInfo : public TargetCodeGenInfo {
public:
  KlaussCPUTargetCodeGenInfo(CodeGenTypes &CGT)
      : TargetCodeGenInfo(std::make_unique<KlaussCPUABIInfo>(CGT)) {}
};

} // namespace

std::unique_ptr<TargetCodeGenInfo>
CodeGen::createKlaussCPUTargetCodeGenInfo(CodeGenModule &CGM) {
  return std::make_unique<KlaussCPUTargetCodeGenInfo>(CGM.getTypes());
}
