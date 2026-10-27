# RUN: not llvm-mc -triple=klausscpu-unknown-elf -filetype=obj %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --implicit-check-not=error:
# RUN: not llvm-mc -triple=klausscpu-unknown-elf -filetype=obj \
# RUN:   -klausscpu-short-forms=false %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --implicit-check-not=error:

# A symbol in an immediate field whose operand has no EncoderMethod (so no
# fixup kind) is rejected by the operand class (KCImmAsmOperand without
# allowSym), with the field's range, instead of reaching the code emitter.
# Constants folded by the parser encode.

        .set    kconst, 5
        addv    r1, r2, kconst
        cmprv   r1, kconst

# CHECK: [[#@LINE+1]]:25: error: immediate must be an integer in the range [0, 4294967295]
        addv    r1, r2, sym
# CHECK: [[#@LINE+1]]:21: error: immediate must be an integer in the range [-2147483648, 2147483647]
        cmprv   r1, sym
# CHECK: [[#@LINE+1]]:25: error: immediate must be an integer in the range [-2147483648, 2147483647]
        ldidx64 r1, r2, sym
# CHECK: [[#@LINE+1]]:25: error: immediate must be an integer in the range [0, 63]
        shlv    r1, r2, sym
# CHECK: [[#@LINE+1]]:17: error: immediate must be an integer in the range [0, 4194303]
        enter   sym
# CHECK: [[#@LINE+1]]:25: error: immediate must be an integer in the range [0, 4294967295]
        addv    r1, r2, later
later:
