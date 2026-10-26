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
# CHECK-DAG: [[#@LINE+1]]:17: error: fixup value out of range for 4-byte field
        call    HUGE
# CHECK-DAG: [[#@LINE+1]]:25: error: symbol difference in a PC-relative operand
        leapc   r1, ext - small
small:
        .fill   0x10000, 1, 0
big:

        .set    HUGE, 0x100000000
