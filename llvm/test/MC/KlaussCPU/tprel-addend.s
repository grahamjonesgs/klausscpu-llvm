# RUN: not llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj %s -o /dev/null 2>&1 | FileCheck %s

# R_KLAUSSCPU_TPREL32 is a signed offset (lld sign-extends it), so an
# addend that only fits as unsigned 32 bits is an error, not wrapped.
  .section .tbss,"awT",@nobits
a: .quad 0
  .text
# CHECK: error: relocation addend out of range
  addi r2, r1, %tprel(a)+0xFFFFFFF0
# CHECK: error: relocation addend out of range
  addi r2, r1, %tprel(a)+0x80000000
# CHECK-NOT: error:
  addi r2, r1, %tprel(a)+0x7FFFFFF0
  addi r2, r1, %tprel(a)-16
