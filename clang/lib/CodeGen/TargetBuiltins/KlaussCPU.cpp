//===-------- KlaussCPU.cpp - Emit LLVM Code for builtins -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This contains code to emit KlaussCPU Builtin calls as LLVM code.
//
//===----------------------------------------------------------------------===//

#include "CodeGenFunction.h"
#include "clang/Basic/TargetBuiltins.h"
#include "llvm/IR/IntrinsicsKlaussCPU.h"

using namespace clang;
using namespace CodeGen;
using namespace llvm;

// The intrinsics work on i64, the only register type; the builtins take and
// return unsigned int (the old enable bit is 0 or 1, ei uses bit 0 only).
Value *CodeGenFunction::EmitKlaussCPUBuiltinExpr(unsigned BuiltinID,
                                                 const CallExpr *E) {
  switch (BuiltinID) {
  default:
    llvm_unreachable("Unexpected KlaussCPU builtin");
  case KlaussCPU::BI__builtin_klausscpu_di: {
    Value *Old = Builder.CreateCall(
        CGM.getIntrinsic(Intrinsic::klausscpu_di), {}, "di");
    return Builder.CreateTrunc(Old, ConvertType(E->getType()));
  }
  case KlaussCPU::BI__builtin_klausscpu_ei: {
    Value *Key = EmitScalarExpr(E->getArg(0));
    Key = Builder.CreateZExt(Key, Int64Ty);
    return Builder.CreateCall(CGM.getIntrinsic(Intrinsic::klausscpu_ei), Key);
  }
  }
}
