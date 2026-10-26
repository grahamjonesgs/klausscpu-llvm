# RUN: llvm-mc -triple=klausscpu -filetype=obj %s -o %t.o
# RUN: llvm-objdump -d %t.o | FileCheck %s

# Code-section alignment padding that is not a multiple of 4 (after data
# such as .byte / .ascii) is zero-filled up to the next word boundary and
# NOP-filled from there.  writeNopData used to fail ("unable to write nop
# sequence") on it.

# CHECK:       0: 00 00 00 6c  nop
# CHECK-NEXT:  4: 01 00 00 00  <unknown>
# CHECK-NEXT:  8: 00 00 00 6c  nop
# CHECK-NEXT:  c: 00 00 00 6c  nop
# CHECK-NEXT: 10: 00 00 80 65  ret
# CHECK-NEXT: 14: 61 62 00 00  <unknown>
# CHECK-NEXT: 18: 00 00 80 65  ret
        .text
        nop
        .byte   1
        .p2align 4
        ret
        .ascii  "ab"
        .p2align 2
        ret
