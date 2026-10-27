# RUN: llvm-mc -triple=klausscpu -filetype=obj %s -o %t.o
# RUN: llvm-objdump -dr %t.o | FileCheck %s
# RUN: llvm-mc -triple=klausscpu -filetype=obj -klausscpu-short-calls=false \
# RUN:   %s -o %t.long.o
# RUN: llvm-objdump -dr --no-show-raw-insn %t.long.o \
# RUN:   | FileCheck %s --check-prefix=LONG
# RUN: llvm-mc -triple=klausscpu -filetype=obj -mattr=-short-calls %s \
# RUN:   -o %t.attr.o
# RUN: cmp %t.long.o %t.attr.o
# RUN: llvm-mc -triple=klausscpu %s | FileCheck %s --check-prefix=ASM

# ISA v3 A1 short call (word 0 0x63000000 | simm18, pushes PC+4): a call to
# a symbol, `call` or `callrel`, is the 1-word short call.  A local callee in
# the same section is resolved here, and the call relaxed to the 2-word CALLREL
# when it is out of reach; any other callee is an R_KLAUSSCPU_CALL18
# relocation at the instruction (the linker adds a thunk when out of
# reach).  A literal displacement and a conditional call stay long.  It
# prints as `callrel`, so the .s reassembles to the same bytes.

# CHECK-LABEL: <f>:
# CHECK-NEXT:   0: 03 00 00 63   callrel 0xc <near>
# CHECK-NEXT:   4: 00 00 00 63   callrel 0x4 <f+0x4>
# CHECK-NEXT:        00000004:  R_KLAUSSCPU_CALL18 ext
# CHECK-NEXT:   8: 00 00 00 63   callrel 0x8 <f+0x8>
# CHECK-NEXT:        00000008:  R_KLAUSSCPU_CALL18 ext+0x10
# LONG-LABEL: <f>:
# LONG-NEXT:    0: callrel 0x18 <near>
# LONG-NEXT:    8: call 0x0 <f>
# LONG-NEXT:        0000000c:  R_KLAUSSCPU_ABS32 ext
# LONG-NEXT:   10: callrel 0x10 <f+0x10>
# LONG-NEXT:        00000014:  R_KLAUSSCPU_PCREL32 ext+0x10
# ASM:      callrel near
# ASM:      callrel ext
# ASM:      callrel ext+16
f:
        callrel near
        call    ext
        callrel ext+16
# CHECK-LABEL: <near>:
# CHECK-NEXT:   c: 00 00 00 63   callrel 0xc <near>
# CHECK-NEXT:        0000000c:  R_KLAUSSCPU_CALL18 .text.other
# CHECK-NEXT:  10: 00 00 00 63   callrel 0x10 <near+0x4>
# CHECK-NEXT:        00000010:  R_KLAUSSCPU_CALL18 *ABS*+0x1000
# CHECK-NEXT:  14: 00 00 00 a3 10 00 00 00   callrel 0x24 <near+0x18>
# CHECK-NEXT:  1c: 00 00 4c a2 00 00 00 00   callne 0x0 <f>
# CHECK-NEXT:        00000020:  R_KLAUSSCPU_ABS32 ext
## A callee 512 KiB ahead: relaxed.
# CHECK-NEXT:  24: 00 00 00 a3 14 00 08 00   callrel 0x80038 <far>
## The last call that reaches: 0x7fffc ahead.
# CHECK-NEXT:  2c: ff ff 01 63   callrel 0x80028 <reach>
near:
        callrel other
        callrel ABSFN
        callrel 16
        callne  ext
        callrel far
        callrel reach
        .fill   0x7fff8, 1, 0
# CHECK:      <reach>:
# CHECK-NEXT: 80028: 00 00 80 65   ret
## Backwards: -0x80000 reaches, -0x8000c does not.
# CHECK-NEXT: 8002c: 00 00 02 63   callrel 0x2c <near+0x20>
# CHECK-NEXT: 80030: 00 00 00 a3 f4 ff f7 ff   callrel 0x24 <near+0x18>
reach:
        ret
        callrel near+0x20
        callrel near+0x18
far:
        ret

        .set ABSFN, 0x1000

        .section .text.other,"ax"
other:
        ret
## A global or weak callee in the same section keeps its relocation, since
## the link may override it; only a local one is resolved here.
# CHECK-LABEL: <other>:
# CHECK-NEXT:   0: 00 00 80 65   ret
# CHECK-NEXT:   4: 00 00 00 63   callrel 0x4 <other+0x4>
# CHECK-NEXT:        00000004:  R_KLAUSSCPU_CALL18 gfn
# CHECK-NEXT:   8: 00 00 00 63   callrel 0x8 <other+0x8>
# CHECK-NEXT:        00000008:  R_KLAUSSCPU_CALL18 wfn
# CHECK-NEXT:   c: fd ff 03 63   callrel 0x0 <other>
        callrel gfn
        callrel wfn
        callrel other
        .globl gfn
gfn:
        ret
        .weak wfn
wfn:
        ret
