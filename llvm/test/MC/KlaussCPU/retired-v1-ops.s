# RUN: not llvm-mc -triple=klausscpu-unknown-elf %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --implicit-check-not=error:

# The ISA v1 LED / 7-segment / RGB peripheral opcodes no longer exist (those
# peripherals are memory-mapped), so their mnemonics do not assemble: their
# v1 words would trap as invalid opcodes on the v2 CPU.

# CHECK: :[[@LINE+1]]:{{[0-9]+}}: error: unrecognised instruction mnemonic
        ledr    r1
# CHECK: :[[@LINE+1]]:{{[0-9]+}}: error: unrecognised instruction mnemonic
        ledv    0xFFFF
# CHECK: :[[@LINE+1]]:{{[0-9]+}}: error: unrecognised instruction mnemonic
        rgb1r   r2
# CHECK: :[[@LINE+1]]:{{[0-9]+}}: error: unrecognised instruction mnemonic
        rgb2r   r3
# CHECK: :[[@LINE+1]]:{{[0-9]+}}: error: unrecognised instruction mnemonic
        rgb1v   5
# CHECK: :[[@LINE+1]]:{{[0-9]+}}: error: unrecognised instruction mnemonic
        rgb2v   5

# The ISA map spells them in upper case; mnemonics are case-insensitive.
# CHECK: :[[@LINE+1]]:{{[0-9]+}}: error: unrecognised instruction mnemonic
        LEDR    r1

# The 7-segment family started with a digit, so it does not even lex as a
# mnemonic any more.
# CHECK: :[[@LINE+1]]:{{[0-9]+}}: error: unexpected token at start of statement
        7segblank
# CHECK: :[[@LINE+1]]:{{[0-9]+}}: error: unexpected token at start of statement
        7seg1r  r1
