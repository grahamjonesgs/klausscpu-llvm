# RUN: llvm-mc -triple=klausscpu-unknown-elf -show-encoding %s | FileCheck %s
# RUN: llvm-mc -triple=klausscpu-unknown-elf -filetype=obj %s -o %t.o
# RUN: llvm-objdump -d %t.o | FileCheck %s --check-prefix=DIS
# RUN: not llvm-mc -triple=klausscpu-unknown-elf --defsym=ERR=1 %s \
# RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR --implicit-check-not=error:

# BEXTR rd, rs1, imm32 and BDEP rd, rs1(base), rs2(source), imm32
# (ISA_ENCODING_V2_MAP §3 class 4): the field descriptor is start [4:0],
# len [12:8].  rd and rs1 are independent; BDEP's source register is rs2
# [3:0].  BEXTR's rs2 field is zero.

# CHECK: bextr r3, r5, 2052         # encoding: [0x50,0x03,0x00,0x93,0x04,0x08,0x00,0x00]
# DIS:   50 03 00 93 04 08 00 00    bextr r3, r5, 2052
        bextr   r3, r5, 0x0804
# CHECK: bdep r3, r5, r7, 2052      # encoding: [0x57,0x03,0x40,0x93,0x04,0x08,0x00,0x00]
# DIS:   57 03 40 93 04 08 00 00    bdep r3, r5, r7, 2052
        bdep    r3, r5, r7, 0x0804
# CHECK: bdep r15, r0, r14, 7967    # encoding: [0x0e,0x0f,0x40,0x93,0x1f,0x1f,0x00,0x00]
# DIS:   0e 0f 40 93 1f 1f 00 00    bdep r15, r0, r14, 7967
        bdep    r15, r0, r14, 0x1F1F

# The old in-place BEXTR spelling still assembles, as rd == rs1.
# CHECK: bextr r4, r4, 2052         # encoding: [0x40,0x04,0x00,0x93,0x04,0x08,0x00,0x00]
# DIS:   40 04 00 93 04 08 00 00    bextr r4, r4, 2052
        bextr   r4, 0x0804

.ifdef ERR
# The old `bdep rd, imm` could not name its source (it deposited r0).
# ERR: [[#@LINE+1]]:21: error: invalid operand for instruction
        bdep    r1, 0x0804
# ERR: [[#@LINE+1]]:25: error: invalid operand for instruction
        bdep    r1, r2, 0x0804
# ERR: [[#@LINE+1]]:29: error: immediate must be an integer in the range [0, 4294967295]
        bdep    r1, r2, r3, -1
.endif
