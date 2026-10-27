# REQUIRES: klausscpu
## Addresses are 32-bit and zero-extended in a register (SETRU, LEAPC), so an
## 8-byte pointer field (R_KLAUSSCPU_ABS64) holds the address zero-extended
## too, also for one at or above 2 GiB (MMIO), although lld hands an
## ELFCLASS32 target the value sign-extended from 32 bits.  The field is the
## exact 64-bit S + A, so a negative one (`(intptr_t)&dat - 0x100000`, dat at
## a low address) stays negative, as the code computes it.  The ABS32 word of
## SETRU / SETR is the low half; SETR's CPU sign-extends it.

# RUN: llvm-mc -triple=klausscpu -filetype=obj %s -o %t.o
# RUN: echo 'SECTIONS { .text 0x20 : { *(.text) } .data 0x100 : { *(.data) } }' > %t.lds
# RUN: ld.lld -T %t.lds %t.o --defsym=mmio=0xF0010000 --defsym=low=0x7FFFFFF0 -o %t
# RUN: llvm-objdump -d --no-show-raw-insn %t | FileCheck %s
# RUN: llvm-objdump -s -j .data %t | FileCheck %s --check-prefix=DATA

# CHECK:      <_start>:
# CHECK-NEXT:   20: setru r1, 4026597376
# CHECK-NEXT:   28: setru r2, 4026597380
# CHECK-NEXT:   30: setr r3, -268369920
# CHECK-NEXT:   38: setru r4, 256

## 0x100: .quad mmio, .quad mmio+8, .quad low, .quad dat, .long mmio,
## .long 0, .quad dat-0x100000, .quad mmio-0x10, .quad low+0x10
# DATA:      0100 000001f0 00000000 080001f0 00000000
# DATA-NEXT: 0110 f0ffff7f 00000000 00010000 00000000
# DATA-NEXT: 0120 000001f0 00000000 0001f0ff ffffffff
# DATA-NEXT: 0130 f0ff00f0 00000000 00000080 00000000

        .text
        .globl _start
_start:
        setru   r1, mmio
        setru   r2, mmio+4
        setr    r3, mmio
        setru   r4, dat

        .data
dat:
        .quad   mmio
        .quad   mmio+8
        .quad   low
        .quad   dat
        .long   mmio
        .long   0
        .quad   dat-0x100000
        .quad   mmio-0x10
        .quad   low+0x10
