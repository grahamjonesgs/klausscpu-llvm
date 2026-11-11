# RUN: llvm-mc -triple=klausscpu-unknown-elf -show-encoding %s \
# RUN:   | FileCheck %s
# RUN: llvm-mc -triple=klausscpu-unknown-elf -filetype=obj %s -o %t.o
# RUN: llvm-objdump -d --no-show-raw-insn %t.o | FileCheck %s --check-prefix=DIS

# DCBZ rs1 = 0x5F002000 | rs1 << 4 (klaussCPU HW_PERF_PLAN.md §1): class 7,
# LEN=01, SIZE=11, MODE=00, A=0, [13]=1; every other field 0.  Encodings
# computed from the plan's template, not from this backend.

# CHECK: dcbz r1                    # encoding: [0x10,0x20,0x00,0x5f]
# DIS: dcbz r1
        dcbz    r1
# CHECK: dcbz r10                   # encoding: [0xa0,0x20,0x00,0x5f]
# DIS-NEXT: dcbz r10
        dcbz    r10
# CHECK: dcbz r15                   # encoding: [0xf0,0x20,0x00,0x5f]
# DIS-NEXT: dcbz r15
        dcbz    r15
# The neighbouring SC.D encoding ([12]=1) still disassembles as sc64.
# CHECK: sc64 r6, r5, r1            # encoding: [0x15,0x16,0x00,0x5f]
# DIS-NEXT: sc64 r6, r5, r1
        sc64    r6, r5, r1
