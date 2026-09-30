# RUN: llvm-mc -triple=klausscpu-unknown-elf -show-encoding %s | FileCheck %s
# ISA v3 D3 W ALU ops (ISA_ENCODING_V2_MAP.md 7.6).

# CHECK: addw r12, r0, r1 # encoding: [0x01,0x0c,0x28,0x44]
addw r12, r0, r1
# CHECK: subw r3, r4, r5 # encoding: [0x45,0x03,0x68,0x44]
subw r3, r4, r5
# CHECK: mulw r12, r0, r1 # encoding: [0x01,0x0c,0x88,0x68]
mulw r12, r0, r1
# CHECK: addiw r12, r0, 1000 # encoding: [0x00,0x0c,0x38,0x88,0xe8,0x03,0x00,0x00]
addiw r12, r0, 1000
