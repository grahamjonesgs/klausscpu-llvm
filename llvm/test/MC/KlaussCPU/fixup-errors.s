# RUN: not llvm-mc -triple=klausscpu -filetype=obj %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --implicit-check-not=error:

# Fixups that cannot be represented are errors, not silently dropped or
# truncated.  The label differences refer forward, so they reach the
# backend's applyFixup instead of being folded by the parser.

        .data
# CHECK-DAG: [[#@LINE+1]]:17: error: 1-byte data relocations not supported
        .byte   ext
# CHECK-DAG: [[#@LINE+1]]:17: error: 2-byte data relocations not supported
        .short  ext
# CHECK-DAG: [[#@LINE+1]]:21: error: fixup value out of range for 1-byte field
        .byte   big - small
# CHECK-DAG: [[#@LINE+1]]:21: error: fixup value out of range for 2-byte field
        .short  big - small
# CHECK-DAG: [[#@LINE+1]]:17: error: fixup value out of range for 4-byte field
        .long   HUGE

        .text
# CHECK-DAG: [[#@LINE+1]]:25: error: PC-relative expression in an absolute operand
        setr    r1, ext - .
## A call to a symbol is a short call: its relocation's 32-bit addend cannot
## hold HUGE either.
# CHECK-DAG: [[#@LINE+1]]:17: error: relocation addend out of range
        call    HUGE
# CHECK-DAG: [[#@LINE+1]]:25: error: symbol difference in a PC-relative operand
        leapc   r1, ext - small
## A fused branch reaches only +-16 KiB, so it cannot name an absolute symbol.
        .set    LOW, 0x1000
# CHECK-DAG: [[#@LINE+1]]:28: error: fused branch target out of range
        fbr     r1, r2, 0, LOW
# CHECK-DAG: [[#@LINE+1]]:27: error: fused branch target out of range
        fbri    r1, 3, 0, -LOW
## ELF32 r_addend is 32 bits: a wider addend must not be truncated.
# CHECK-DAG: [[#@LINE+1]]:17: error: relocation addend out of range
        callrel HUGE
# CHECK-DAG: [[#@LINE+1]]:25: error: relocation addend out of range
        leapc   r1, ext + 0x100000000
# CHECK-DAG: [[#@LINE+1]]:21: error: relocation addend out of range
        call    ext + 0x100000000
small:
        .fill   0x10000, 1, 0
big:

        .set    HUGE, 0x100000000

        .data
## 32-bit fields need the addend only modulo 2^32; 8-byte ones sign-extend it.
        .long   ext + 0x80000000
# CHECK-DAG: [[#@LINE+1]]:21: error: relocation addend out of range
        .quad   ext + 0x80000000
