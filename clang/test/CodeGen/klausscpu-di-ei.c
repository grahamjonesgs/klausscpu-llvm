// REQUIRES: klausscpu-registered-target
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -S -O2 -o - %s | FileCheck %s --check-prefix=ASM

// __builtin_klausscpu_di() disables interrupts and returns the old enable bit;
// __builtin_klausscpu_ei(k) sets it to k & 1.  The key is an unsigned int; the
// intrinsics work on i64, the only register type.

// CHECK-LABEL: define {{.*}} @crit(
// CHECK:         [[D:%.*]] = call i64 @llvm.klausscpu.di()
// CHECK:         [[K:%.*]] = trunc i64 [[D]] to i32
// CHECK:         [[Z:%.*]] = zext i32 {{.*}} to i64
// CHECK:         call void @llvm.klausscpu.ei(i64 [[Z]])
// ASM-LABEL: crit:
// ASM:         di
// ASM:         ei
int crit(volatile int *p) {
  unsigned k = __builtin_klausscpu_di();
  int v = *p;
  *p = v + 1;
  __builtin_klausscpu_ei(k);
  return v;
}
