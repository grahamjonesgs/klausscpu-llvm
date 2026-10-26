// REQUIRES: klausscpu-registered-target
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -emit-llvm %s -o - | FileCheck %s

// Most KlaussCPU ALU instructions set the condition flags, so every asm
// statement clobbers FLAGS whether or not it lists "cc".

// CHECK-LABEL: @no_clobber(
// CHECK: call i64 asm "addr $0, $1, $2", "=r,r,r,~{flags}"(
long no_clobber(long a, long b) {
  long t;
  __asm__("addr %0, %1, %2" : "=r"(t) : "r"(a), "r"(b));
  return t;
}

// CHECK-LABEL: @cc_clobber(
// CHECK: call void asm sideeffect "nop", "~{cc},~{flags}"()
void cc_clobber(void) { __asm__ volatile("nop" ::: "cc"); }
