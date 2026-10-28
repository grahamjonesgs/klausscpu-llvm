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
// The integer-only RISC-V LP64 convention with 64-bit GPRs (the backend's
// KlaussCPUCallingConv.td assigns the registers; ABI.md in the backend
// describes the whole ABI):
//   - an integer narrower than 64 bits is extended to 64 by the producer
//     (the caller for an argument, the callee for a return value): an i32
//     is sign-extended even when unsigned, as on RV64, narrower integers by
//     their signedness;
//   - an aggregate of up to 16 bytes is passed and returned directly,
//     coerced to an integer of its size (up to 8 bytes), i128 (16-byte
//     alignment) or [2 x i64]: one or two GPRs or stack slots;
//   - a larger aggregate is passed by reference to a copy the caller makes,
//     and returned through a hidden sret pointer.
// The classification does not depend on how many registers are left or on
// whether an argument is named: the backend puts what does not fit in R0-R3
// on the stack, and a variadic callee stores its unnamed register arguments
// next to the stack ones.
//===----------------------------------------------------------------------===//

namespace {

class KlaussCPUABIInfo : public DefaultABIInfo {
public:
  KlaussCPUABIInfo(CodeGenTypes &CGT) : DefaultABIInfo(CGT) {}

  void computeInfo(CGFunctionInfo &FI) const override {
    if (!getCXXABI().classifyReturnType(FI))
      FI.getReturnInfo() = classifyReturnType(FI.getReturnType());
    for (auto &Arg : FI.arguments())
      Arg.info = classifyArgumentType(Arg.type);
  }

  ABIArgInfo classifyArgumentType(QualType Ty) const;
  ABIArgInfo classifyReturnType(QualType RetTy) const;

  // va_list is a plain pointer to 8-byte slots: a variadic function stores
  // its unnamed register arguments in the home area its caller reserved,
  // right below the stack arguments, each argument takes as many slots as it
  // needs, and nothing is aligned beyond 8.  An aggregate over 16 bytes is passed by reference, so
  // its slot holds its address.  The pointer bump is plain IR, so the
  // optimiser can keep the va_list pointer in a register (the generic va_arg
  // instruction is opaque to it).
  RValue EmitVAArg(CodeGenFunction &CGF, Address VAListAddr, QualType Ty,
                   AggValueSlot Slot) const override {
    ABIArgInfo AI = classifyArgumentType(Ty);
    if (AI.isIgnore())
      return Slot.asRValue();
    return emitVoidPtrVAArg(
        CGF, VAListAddr, Ty, AI.isIndirect(),
        getContext().getTypeInfoInChars(Ty),
        CharUnits::fromQuantity(llvm::KlaussCPUABI::SlotSize),
        /*AllowHigherAlign=*/false, Slot);
  }

private:
  static constexpr unsigned GPRBits = 64;

  ABIArgInfo extendType(QualType Ty, llvm::Type *CoerceTy) const {
    // Every i32 is sign-extended, also an unsigned one (as on RV64): the W
    // instructions and LDIDX32_S produce that form, and a 32-bit unsigned
    // compare or zero-extension works on it unchanged.
    if (getContext().getTypeSize(Ty) == 32)
      return ABIArgInfo::getSignExtend(Ty, CoerceTy);
    return ABIArgInfo::getExtend(Ty, CoerceTy);
  }
};

ABIArgInfo KlaussCPUABIInfo::classifyArgumentType(QualType Ty) const {
  Ty = useFirstFieldIfTransparentUnion(Ty);

  // A record with a non-trivial copy constructor or destructor is always
  // passed by reference.
  if (CGCXXABI::RecordArgABI RAA = getRecordArgABI(Ty, getCXXABI()))
    return getNaturalAlignIndirect(
        Ty, /*AddrSpace=*/getDataLayout().getAllocaAddrSpace(),
        /*ByVal=*/RAA == CGCXXABI::RAA_DirectInMemory);

  uint64_t Size = getContext().getTypeSize(Ty);

  // An empty record of size zero (GNU C `struct {}`) takes no register or
  // slot.  C++ gives an empty class size 1, so it is passed as an i8.
  if (isEmptyRecord(getContext(), Ty, true) && Size == 0)
    return ABIArgInfo::getIgnore();

  if (!isAggregateTypeForABI(Ty) && !Ty->isVectorType()) {
    if (const auto *ED = Ty->getAsEnumDecl())
      Ty = ED->getIntegerType();

    // Integers narrower than a GPR are extended to its width.
    if (Size < GPRBits && Ty->isIntegralOrEnumerationType())
      return extendType(Ty, CGT.ConvertType(Ty));

    return ABIArgInfo::getDirect();
  }

  // Aggregates (and GNU vectors) of up to 16 bytes go in one or two GPRs or
  // stack slots, coerced to integers.
  if (Size <= llvm::KlaussCPUABI::MaxDirectAggregateSize * 8) {
    if (Size <= GPRBits)
      return ABIArgInfo::getDirect(
          llvm::IntegerType::get(getVMContext(), Size));
    if (getContext().getTypeAlign(Ty) == 2 * GPRBits)
      return ABIArgInfo::getDirect(
          llvm::IntegerType::get(getVMContext(), 2 * GPRBits));
    return ABIArgInfo::getDirect(llvm::ArrayType::get(
        llvm::IntegerType::get(getVMContext(), GPRBits), 2));
  }

  return getNaturalAlignIndirect(
      Ty, /*AddrSpace=*/getDataLayout().getAllocaAddrSpace(),
      /*ByVal=*/false);
}

ABIArgInfo KlaussCPUABIInfo::classifyReturnType(QualType RetTy) const {
  if (RetTy->isVoidType())
    return ABIArgInfo::getIgnore();
  // The same rules: what is passed in up to two GPRs comes back in R12 and
  // R11; what is passed by reference is returned through an sret pointer.
  return classifyArgumentType(RetTy);
}

class KlaussCPUTargetCodeGenInfo : public TargetCodeGenInfo {
public:
  KlaussCPUTargetCodeGenInfo(CodeGenTypes &CGT)
      : TargetCodeGenInfo(std::make_unique<KlaussCPUABIInfo>(CGT)) {}

  // A call through a declaration without a prototype (K&R `long f();`) is
  // made as a variadic call, which reserves the R0-R3 home area, when all its
  // arguments are in registers: then a variadic callee (undefined behaviour
  // in C, but it worked with ABI version 0 and works on RISC-V and x86-64)
  // stores them in the home area instead of over the caller's frame, and a
  // non-variadic callee, which reads nothing from the stack, is unaffected.
  // With stack arguments the call stays non-variadic, so they are where a
  // non-variadic callee, the only kind C allows here, expects them.
  bool isNoProtoCallVariadic(const CallArgList &Args,
                             const FunctionNoProtoType *FnType) const override {
    const auto &ABI = getABIInfo<KlaussCPUABIInfo>();
    ASTContext &Ctx = ABI.getContext();
    // The hidden sret pointer takes R0.
    unsigned Words =
        ABI.classifyReturnType(FnType->getReturnType()).isIndirect() ? 1 : 0;
    for (const CallArg &Arg : Args) {
      ABIArgInfo AI = ABI.classifyArgumentType(Arg.Ty);
      if (AI.isIgnore())
        continue;
      Words += AI.isIndirect()
                   ? 1
                   : llvm::divideCeil(Ctx.getTypeSize(Arg.Ty),
                                      llvm::KlaussCPUABI::SlotSize * 8);
    }
    return Words <= llvm::KlaussCPUABI::NumArgGPRs;
  }
};

} // namespace

std::unique_ptr<TargetCodeGenInfo>
CodeGen::createKlaussCPUTargetCodeGenInfo(CodeGenModule &CGM) {
  return std::make_unique<KlaussCPUTargetCodeGenInfo>(CGM.getTypes());
}
