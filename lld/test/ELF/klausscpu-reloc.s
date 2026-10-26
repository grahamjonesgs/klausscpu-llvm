# REQUIRES: klausscpu
## PC-relative instruction and data relocations.  The hardware adds a
## PC-relative word-1 field to the address of the instruction itself, so
## R_KLAUSSCPU_PCREL32 stores S + A - P + 4 (P = instr + 4).  Data differences
## (PC32 / PC64) store S + A - P exactly.  Calls, jumps and LEAPC to absolute
## symbols (.set, --defsym) must reach the absolute address.

# RUN: llvm-mc -triple=klausscpu -filetype=obj %s -o %t.o
# RUN: echo 'SECTIONS { .text 0x20 : { *(.text) } .data 0x100 : { *(.data) } }' > %t.lds
# RUN: ld.lld -T %t.lds %t.o --defsym=ext=0x2000 -o %t
# RUN: llvm-objdump -d --no-show-raw-insn %t | FileCheck %s
# RUN: llvm-objdump -s -j .data %t | FileCheck %s --check-prefix=DATA

# CHECK:      <_start>:
# CHECK-NEXT:   20: leapc r1, 0x1000
# CHECK-NEXT:   28: callrel 0x1000
# CHECK-NEXT:   30: jmprel 0x1000
# CHECK-NEXT:   38: leapc r1, 0x1008
# CHECK-NEXT:   40: callrel 0x2000
# CHECK-NEXT:   48: leapc r2, 0x2010
# CHECK-NEXT:   50: leapc r3, 0x100{{$}}

## 0x100: .long ext - .        = 0x2000 - 0x100 = 0x1f00
## 0x104: .quad ext - .        = 0x2000 - 0x104 = 0x1efc
## 0x10c: .quad _start - .     = 0x20 - 0x10c = -0xec
## 0x114: .long _start - .     = 0x20 - 0x114 = -0xf4
# DATA:      0100 001f0000 fc1e0000 00000000 14ffffff
# DATA-NEXT: 0110 ffffffff 0cffffff

        .text
        .globl _start
_start:
        .set ROMFN, 0x1000
        leapc   r1, ROMFN
        callrel ROMFN
        jmprel  ROMFN
        leapc   r1, ROMFN+8
        callrel ext
        leapc   r2, ext+16
        leapc   r3, dat

        .data
dat:
        .long   ext - .
        .quad   ext - .
        .quad   _start - .
        .long   _start - .
