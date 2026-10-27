# RUN: not llvm-mc -triple=klausscpu-unknown-elf -filetype=obj %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --implicit-check-not=error:
# RUN: not llvm-mc -triple=klausscpu-unknown-elf -filetype=obj \
# RUN:   -klausscpu-short-forms=false %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --implicit-check-not=error:

# A symbol in an immediate field whose operand has no EncoderMethod (so no
# fixup kind) is diagnosed by the code emitter instead of crashing it.  The
# parser still accepts the expression; a constant-only operand class would
# reject it earlier (review issue 9).  Constants folded by the parser encode.

        .set    kconst, 5
        addv    r1, r2, kconst
        cmprv   r1, kconst

# CHECK: [[#@LINE+1]]:9: error: operand must be a constant: no relocation exists for a symbol in this field
        addv    r1, r2, sym
# CHECK: [[#@LINE+1]]:9: error: operand must be a constant: no relocation exists for a symbol in this field
        cmprv   r1, sym
# CHECK: [[#@LINE+1]]:9: error: operand must be a constant: no relocation exists for a symbol in this field
        ldidx64 r1, r2, sym
# CHECK: [[#@LINE+1]]:9: error: operand must be a constant: no relocation exists for a symbol in this field
        shlv    r1, r2, sym
# CHECK: [[#@LINE+1]]:9: error: operand must be a constant: no relocation exists for a symbol in this field
        enter   sym
# CHECK: [[#@LINE+1]]:9: error: operand must be a constant: no relocation exists for a symbol in this field
        addv    r1, r2, later
later:
