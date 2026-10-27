# RUN: llvm-mc -triple=klausscpu-unknown-elf -show-encoding %s | FileCheck %s
# RUN: not llvm-mc -triple=klausscpu-unknown-elf --defsym=ERR=1 %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=ERR --implicit-check-not=error:

# The fused-branch condition (PRED | INV << 3) is a 4-bit constant with
# PRED 0..4 (the CPU traps on PRED 5..7) and the fbri immediate a signed 4-bit
# constant.  Anything else is rejected by the matcher with the operand
# class's diagnostic; an expression there used to reach the encoder and
# assert.  A numeric target is a byte displacement: a multiple of 4 that fits
# the 13-bit word displacement.

# CHECK: fbr r1, r2, 12, lab  # encoding: [0x12,A,0b010AAAAA,0x76]
        fbr     r1, r2, 12, lab
# CHECK: fbri r1, -8, 0, lab  # encoding: [0x18,A,0b001AAAAA,0x74]
        fbri    r1, -8, 0, lab
# CHECK: fbri r1, 7, 8, lab   # encoding: [0x17,A,0b011AAAAA,0x74]
        fbri    r1, 7, 8, lab
# CHECK: fbr r1, r2, 4, lab   # encoding: [0x12,A,0b000AAAAA,0x76]
        fbr     r1, r2, 4, lab
lab:
        ret

.ifdef ERR
# ERR: [[#@LINE+1]]:25: error: fused-branch condition must be an integer in the range [0, 4] or [8, 12]
        fbr     r1, r2, eq, lab
# ERR: [[#@LINE+1]]:25: error: fused-branch condition must be an integer in the range [0, 4] or [8, 12]
        fbr     r1, r2, 16, lab
# ERR: [[#@LINE+1]]:25: error: fused-branch condition must be an integer in the range [0, 4] or [8, 12]
        fbr     r1, r2, -1, lab
# ERR: [[#@LINE+1]]:25: error: fused-branch condition must be an integer in the range [0, 4] or [8, 12]
        fbr     r1, r2, 5, lab
# ERR: [[#@LINE+1]]:25: error: fused-branch condition must be an integer in the range [0, 4] or [8, 12]
        fbr     r1, r2, 6, lab
# ERR: [[#@LINE+1]]:25: error: fused-branch condition must be an integer in the range [0, 4] or [8, 12]
        fbr     r1, r2, 7, lab
# ERR: [[#@LINE+1]]:25: error: fused-branch condition must be an integer in the range [0, 4] or [8, 12]
        fbr     r1, r2, 13, lab
# ERR: [[#@LINE+1]]:25: error: fused-branch condition must be an integer in the range [0, 4] or [8, 12]
        fbr     r1, r2, 14, lab
# ERR: [[#@LINE+1]]:25: error: fused-branch condition must be an integer in the range [0, 4] or [8, 12]
        fbr     r1, r2, 15, lab
# ERR: [[#@LINE+1]]:24: error: fused-branch condition must be an integer in the range [0, 4] or [8, 12]
        fbri    r1, 0, 13, lab
# ERR: [[#@LINE+1]]:21: error: immediate must be an integer in the range [-8, 7]
        fbri    r1, 8, 0, lab
# ERR: [[#@LINE+1]]:21: error: immediate must be an integer in the range [-8, 7]
        fbri    r1, sym, 0, lab
# ERR: [[#@LINE+1]]:24: error: fused-branch condition must be an integer in the range [0, 4] or [8, 12]
        fbri    r1, 0, lab, lab
# ERR: [[#@LINE+1]]:28: error: immediate must be a multiple of 4 in the range [-16384, 16380] or a symbol
        fbr     r1, r2, 0, 2
# ERR: [[#@LINE+1]]:28: error: immediate must be a multiple of 4 in the range [-16384, 16380] or a symbol
        fbr     r1, r2, 0, 16384
# ERR: [[#@LINE+1]]:27: error: immediate must be a multiple of 4 in the range [-16384, 16380] or a symbol
        fbri    r1, 0, 0, -16388
.endif
