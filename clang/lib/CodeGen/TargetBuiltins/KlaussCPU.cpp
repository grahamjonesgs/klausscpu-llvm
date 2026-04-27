//===-- KlaussCPU.cpp - Emit LLVM IR for KlaussCPU builtins ---------------===//
//
// Lowers __builtin_klausscpu_* to the corresponding LLVM intrinsics.
// The intrinsics are defined in IntrinsicsKlaussCPU.td and lowered to
// machine instructions in KlaussCPUISelDAGToDAG.cpp.
//
//===----------------------------------------------------------------------===//

#include "CGBuiltin.h"
#include "clang/Basic/TargetBuiltins.h"
#include "llvm/IR/IntrinsicsKlaussCPU.h"

using namespace clang;
using namespace CodeGen;
using namespace llvm;

llvm::Value *
CodeGenFunction::EmitKlaussCPUBuiltinExpr(unsigned BuiltinID,
                                           const CallExpr *E) {
  switch (BuiltinID) {

  // void __builtin_klausscpu_txr(unsigned long long val)
  // → call void @llvm.klausscpu.txr(i64 %val)
  case KlaussCPU::BI__builtin_klausscpu_txr: {
    Value *Val = EmitScalarExpr(E->getArg(0));
    Function *F = CGM.getIntrinsic(Intrinsic::klausscpu_txr);
    return Builder.CreateCall(F, {Val});
  }

  // void __builtin_klausscpu_txmemr(const void *ptr)
  // → call void @llvm.klausscpu.txmemr(ptr %ptr)
  case KlaussCPU::BI__builtin_klausscpu_txmemr: {
    Value *Ptr = EmitScalarExpr(E->getArg(0));
    Function *F = CGM.getIntrinsic(Intrinsic::klausscpu_txmemr);
    return Builder.CreateCall(F, {Ptr});
  }

  // void __builtin_klausscpu_txcharmemr(const void *ptr)
  // → call void @llvm.klausscpu.txcharmemr(ptr %ptr)
  case KlaussCPU::BI__builtin_klausscpu_txcharmemr: {
    Value *Ptr = EmitScalarExpr(E->getArg(0));
    Function *F = CGM.getIntrinsic(Intrinsic::klausscpu_txcharmemr);
    return Builder.CreateCall(F, {Ptr});
  }

  // void __builtin_klausscpu_txstrmemr(const char *str)
  // → call void @llvm.klausscpu.txstrmemr(ptr %str)
  case KlaussCPU::BI__builtin_klausscpu_txstrmemr: {
    Value *Ptr = EmitScalarExpr(E->getArg(0));
    Function *F = CGM.getIntrinsic(Intrinsic::klausscpu_txstrmemr);
    return Builder.CreateCall(F, {Ptr});
  }

  // unsigned long long __builtin_klausscpu_rxrb(void)
  // → %v = call i64 @llvm.klausscpu.rxrb()
  case KlaussCPU::BI__builtin_klausscpu_rxrb: {
    Function *F = CGM.getIntrinsic(Intrinsic::klausscpu_rxrb);
    return Builder.CreateCall(F);
  }

  // unsigned long long __builtin_klausscpu_rxrnb(void)
  // → %v = call i64 @llvm.klausscpu.rxrnb()
  case KlaussCPU::BI__builtin_klausscpu_rxrnb: {
    Function *F = CGM.getIntrinsic(Intrinsic::klausscpu_rxrnb);
    return Builder.CreateCall(F);
  }

  // void __builtin_klausscpu_newline(void)
  // → call void @llvm.klausscpu.newline()
  case KlaussCPU::BI__builtin_klausscpu_newline: {
    Function *F = CGM.getIntrinsic(Intrinsic::klausscpu_newline);
    return Builder.CreateCall(F);
  }

  // void __builtin_klausscpu_leds(unsigned long long val)
  case KlaussCPU::BI__builtin_klausscpu_leds: {
    Value *Val = EmitScalarExpr(E->getArg(0));
    Function *F = CGM.getIntrinsic(Intrinsic::klausscpu_leds);
    return Builder.CreateCall(F, {Val});
  }

  // void __builtin_klausscpu_seg7_1(unsigned long long val)
  case KlaussCPU::BI__builtin_klausscpu_seg7_1: {
    Value *Val = EmitScalarExpr(E->getArg(0));
    Function *F = CGM.getIntrinsic(Intrinsic::klausscpu_seg7_1);
    return Builder.CreateCall(F, {Val});
  }

  // void __builtin_klausscpu_seg7_2(unsigned long long val)
  case KlaussCPU::BI__builtin_klausscpu_seg7_2: {
    Value *Val = EmitScalarExpr(E->getArg(0));
    Function *F = CGM.getIntrinsic(Intrinsic::klausscpu_seg7_2);
    return Builder.CreateCall(F, {Val});
  }

  // void __builtin_klausscpu_seg7(unsigned long long val)
  case KlaussCPU::BI__builtin_klausscpu_seg7: {
    Value *Val = EmitScalarExpr(E->getArg(0));
    Function *F = CGM.getIntrinsic(Intrinsic::klausscpu_seg7);
    return Builder.CreateCall(F, {Val});
  }

  // void __builtin_klausscpu_seg7blank(void)
  case KlaussCPU::BI__builtin_klausscpu_seg7blank: {
    Function *F = CGM.getIntrinsic(Intrinsic::klausscpu_seg7blank);
    return Builder.CreateCall(F);
  }

  // void __builtin_klausscpu_delayv(unsigned int cycles)
  case KlaussCPU::BI__builtin_klausscpu_delayv: {
    Value *Cycles = EmitScalarExpr(E->getArg(0));
    Function *F = CGM.getIntrinsic(Intrinsic::klausscpu_delayv);
    return Builder.CreateCall(F, {Cycles});
  }

  default:
    return nullptr;
  }
}
