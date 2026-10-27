# REQUIRES: klausscpu
## R_KLAUSSCPU_CALL18: the ISA v3 short call (word 0 0x63000000) holds a
## simm18 word displacement (S + A - P) / 4 in word0[17:0], P = the
## instruction.  It reaches [-0x80000, 0x7fffc]; a callee further away is
## called through a thunk, `jmprel S + A`, which lld places within reach.
## Calls to the same callee share a thunk; a local callee (a section symbol
## plus addend) and an undefined weak one (address 0) get one too.

# RUN: llvm-mc -triple=klausscpu -filetype=obj %s -o %t.o
# RUN: echo 'SECTIONS { .text.back 0x80008 : { *(.text.back) } \
# RUN:   .text 0x100000 : { *(.text.a) } \
# RUN:   .text.fwd 0x17fffc : { *(.text.fwd) } }' > %t.lds
# RUN: ld.lld -T %t.lds %t.o -o %t
# RUN: llvm-objdump -d %t | FileCheck %s
# RUN: ld.lld -r %t.o -o %t.r.o
# RUN: llvm-readobj -r %t.r.o | FileCheck %s --check-prefix=REL
# RUN: llvm-mc -triple=klausscpu -filetype=obj --defsym=ODD=1 %s -o %t.odd.o
# RUN: not ld.lld -T %t.lds %t.odd.o --defsym=odd=0x100102 -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=ERR

# CHECK:      <_start>:
## +0x7fffc: the furthest forward reach.
# CHECK-NEXT: 100000: ff ff 01 63 callrel 0x17fffc <fwd_max>
## +0x80000: one word too far.
# CHECK-NEXT: 100004: 0b 00 00 63 callrel 0x100030 <__KlaussCPUThunk_fwd_over>
## -0x80000 reaches, -0x80004 does not.
# CHECK-NEXT: 100008: 00 00 02 63 callrel 0x80008 <back>
# CHECK-NEXT: 10000c: 0b 00 00 63 callrel 0x100038 <__KlaussCPUThunk_back>
# CHECK-NEXT: 100010: 0c 00 00 63 callrel 0x100040 <__KlaussCPUThunk_weakf>
## A local callee's section symbol has no name: its thunk is named after the
## section and the offset.
# CHECK-NEXT: 100014: 0d 00 00 63 callrel 0x100048 <__KlaussCPUThunk_.text.back+0x4>
## Resolved by the assembler: no relocation.
# CHECK-NEXT: 100018: 05 00 00 63 callrel 0x10002c <near+0x4>
## A second call to fwd_over is in reach.
# CHECK-NEXT: 10001c: fa ff 01 63 callrel 0x180004 <fwd_over>
# CHECK-NEXT: 100020: 0c 00 00 63 callrel 0x100050 <__KlaussCPUThunk_.text.back+0x8>
# CHECK-NEXT: 100024: 00 00 01 6c halt

# CHECK:      <__KlaussCPUThunk_fwd_over>:
# CHECK-NEXT: 100030: 00 00 00 a1 d4 ff 07 00 jmprel 0x180004 <fwd_over>
# CHECK:      <__KlaussCPUThunk_back>:
# CHECK-NEXT: 100038: 00 00 00 a1 d0 ff f7 ff jmprel 0x80008 <back>
# CHECK:      <__KlaussCPUThunk_weakf>:
# CHECK-NEXT: 100040: 00 00 00 a1 c0 ff ef ff jmprel 0x0
# CHECK:      <__KlaussCPUThunk_.text.back+0x4>:
# CHECK-NEXT: 100048: 00 00 00 a1 c4 ff f7 ff jmprel 0x8000c <local_back>
# CHECK:      <__KlaussCPUThunk_.text.back+0x8>:
# CHECK-NEXT: 100050: 00 00 00 a1 c0 ff f7 ff jmprel 0x80010 <local_back2>

## A relocatable link keeps the relocations.
# REL:      .rela.text.a {
# REL-NEXT:   0x0 R_KLAUSSCPU_CALL18 fwd_max 0x0
# REL-NEXT:   0x4 R_KLAUSSCPU_CALL18 fwd_over 0x0
# REL-NEXT:   0x8 R_KLAUSSCPU_CALL18 back 0x0
# REL-NEXT:   0xC R_KLAUSSCPU_CALL18 back 0x0
# REL-NEXT:   0x10 R_KLAUSSCPU_CALL18 weakf 0x0
# REL-NEXT:   0x14 R_KLAUSSCPU_CALL18 .text.back 0x4
# REL-NEXT:   0x1C R_KLAUSSCPU_CALL18 fwd_over 0x0
# REL-NEXT:   0x20 R_KLAUSSCPU_CALL18 .text.back 0x8
# REL-NEXT: }

# ERR: error: {{.*}}:(.text.a+0x28): improper alignment for relocation R_KLAUSSCPU_CALL18: 0x{{[0-9A-F]+}} is not aligned to 4 bytes

        .section .text.a,"ax"
        .globl _start
_start:
        callrel fwd_max
        callrel fwd_over
        callrel back
        callrel back
        callrel weakf
        callrel local_back
        callrel near+4
        callrel fwd_over
        callrel local_back2
        halt
.ifdef ODD
        callrel odd
.endif
near:
        ret
        ret

        .section .text.fwd,"ax"
        .globl fwd_max, fwd_over
fwd_max:
        ret
        ret
fwd_over:
        ret

        .section .text.back,"ax"
        .globl back
back:
        ret
local_back:
        ret
local_back2:
        ret

        .weak weakf
