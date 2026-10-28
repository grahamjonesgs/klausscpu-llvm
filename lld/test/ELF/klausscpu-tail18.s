# REQUIRES: klausscpu
## A tail call's short jump (`tail`: the short jump with an
## R_KLAUSSCPU_CALL18 relocation) is linked like a short call: in reach it is
## patched in place, out of reach it jumps to the same `jmprel S + A` thunk,
## and the callee returns straight to the tail-calling function's caller.

# RUN: llvm-mc -triple=klausscpu -filetype=obj %s -o %t.o
# RUN: echo 'SECTIONS { .text 0x100000 : { *(.text.a) } \
# RUN:   .text.fwd 0x17fffc : { *(.text.fwd) } }' > %t.lds
# RUN: ld.lld -T %t.lds %t.o -o %t
# RUN: llvm-objdump -d %t | FileCheck %s

# CHECK:      <_start>:
# CHECK-NEXT: 100000: ff ff 01 61 jmprel 0x17fffc <fwd_max>
# CHECK-NEXT: 100004: 01 00 00 61 jmprel 0x100008 <__KlaussCPUThunk_fwd_over>
# CHECK:      <__KlaussCPUThunk_fwd_over>:
# CHECK-NEXT: 100008: 00 00 00 a1 fc ff 07 00 jmprel 0x180004 <fwd_over>

        .section .text.a,"ax"
        .globl _start
_start:
        tail    fwd_max
        tail    fwd_over

        .section .text.fwd,"ax"
        .globl fwd_max, fwd_over
fwd_max:
        ret
        .fill   4, 1, 0
fwd_over:
        ret
