# RUN: llvm-mc -triple=klausscpu -filetype=obj %s -o %t.o
# RUN: llvm-objdump -s -j .data %t.o | FileCheck %s

# GNU as data sizes, as on RISC-V: .half/.hword 2 bytes, .word 4,
# .dword 8 (the Linux kernel's and musl's assembly use them).

        .data
        .half   0x1122
        .hword  0x3344
        .word   0x55667788
        .dword  0x0102030405060708

# CHECK:      Contents of section .data:
# CHECK-NEXT:  0000 22114433 88776655 08070605 04030201
