# REQUIRES: klausscpu
## KlaussCPU code starts at 0x20, so references from .debug_* to a section
## discarded by --gc-sections resolve to the -1 tombstone (0xffffffff), not
## 0, which would make dead functions overlap live code.  .debug_ranges /
## .debug_loc keep the generic tombstone of 1.

# RUN: llvm-mc -triple=klausscpu -filetype=obj %s -o %t.o
# RUN: ld.lld --gc-sections -e live %t.o -o %t
# RUN: llvm-objdump -s -j .debug_info -j .debug_ranges %t | FileCheck %s

# CHECK:      Contents of section .debug_info:
# CHECK-NEXT:  0000 ffffffff [[LIVE:[0-9a-f]{8}]]
# CHECK:      Contents of section .debug_ranges:
# CHECK-NEXT:  0000 01000000

        .section .text.dead,"ax",@progbits
dead:
        nop

        .section .text.live,"ax",@progbits
        .globl live
live:
        ret

        .section .debug_info,"",@progbits
        .long dead
        .long live

        .section .debug_ranges,"",@progbits
        .long dead
