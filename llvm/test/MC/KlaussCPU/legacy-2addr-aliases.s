# RUN: llvm-mc -triple=klausscpu-unknown-elf -show-encoding %s | FileCheck %s
#
# ISA v2 unary / immediate / shift-by-N ops are 3-operand (rd, rs1, ...).
# The pre-v2 in-place spellings still assemble, as rd == rs1.

# CHECK: incr r1, r1       # encoding: [0x10,0x01,0x88,0x57]
incr r1
# CHECK: incr r1, r2       # encoding: [0x20,0x01,0x88,0x57]
incr r1, r2
# CHECK: addv r3, r3, 5    # encoding: [0x30,0x03,0x20,0x88,0x05,0x00,0x00,0x00]
addv r3, 5
# CHECK: addv r3, r4, 5    # encoding: [0x40,0x03,0x20,0x88,0x05,0x00,0x00,0x00]
addv r3, r4, 5
# CHECK: shlv r5, r5, 3    # encoding: [0x50,0xc5,0x21,0x50]
shlv r5, 3
# CHECK: shlv r5, r6, 3    # encoding: [0x60,0xc5,0x21,0x50]
shlv r5, r6, 3
# CHECK: sextw r7, r7      # encoding: [0x70,0x07,0x20,0x55]
sextw r7
# CHECK: ldidx64r r1, r2, r3 # encoding: [0x23,0x01,0x60,0x5b]
ldidx64r r1, r2, r3
# CHECK: stidx64r r4, r5, r6 # encoding: [0x56,0x04,0x60,0x5f]
stidx64r r4, r5, r6
