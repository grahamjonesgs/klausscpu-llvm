# REQUIRES: klausscpu
## Gaps between code sections are filled with an architecturally reserved
## word (LEN = 00, class 0, KlaussCPUABI::InvalidWord), which traps as an
## invalid opcode: not TRAP (the debugger's breakpoint), not HALT (a clean
## exit).  The assembler pads inside a section with NOPs instead.

# RUN: llvm-mc -triple=klausscpu -filetype=obj %s -o %t.o
# RUN: ld.lld %t.o -o %t
# RUN: llvm-objdump -d -z %t | FileCheck %s

# CHECK:      <_start>:
# CHECK-NEXT:   00 00 01 6c   halt
# CHECK-NEXT:   00 00 00 6c   nop
# CHECK-NEXT:   00 00 00 00   <unknown>
# CHECK-NEXT:   00 00 00 00   <unknown>
# CHECK-EMPTY:
# CHECK-NEXT: <f>:
# CHECK-NEXT:   00 00 80 65   ret

  .section .text._start,"ax",@progbits
  .globl _start
  .p2align 3
_start:
  halt
  .p2align 3
  .section .text.f,"ax",@progbits
  .p2align 4
f:
  ret
