# RUN: not llvm-mc -triple=klausscpu-unknown-linux-musl %s 2>&1 \
# RUN:   | FileCheck %s --implicit-check-not=error:
# RUN: not llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj \
# RUN:   --defsym=OBJ=1 %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=OBJ --implicit-check-not=error:

# Where `%tprel` is allowed (KlaussCPU TLS local exec): the immediate of
# addi, setr and the indexed loads / stores, around a TLS symbol.

.ifndef OBJ
## A plain symbol is no TPREL operand: there is no absolute relocation for
## an addi / indexed-access immediate.
# CHECK: [[@LINE+1]]:25: error: immediate must be an integer in the range [-2147483648, 2147483647] or %tprel(symbol)
        addi    r2, r1, sym
# CHECK: [[@LINE+1]]:25: error: immediate must be an integer in the range [-2147483648, 2147483647] or %tprel(symbol)
        ldidx64 r3, r1, sym+8
# CHECK: [[@LINE+2]]:26: error: unknown relocation specifier '%lo'
# CHECK: [[@LINE+1]]:25: error: failed to parse operand
        addi    r2, r1, %lo(sym)
# CHECK: [[@LINE+2]]:32: error: expected '(' after %tprel
# CHECK: [[@LINE+1]]:32: error: failed to parse operand
        addi    r2, r1, %tprel sym
# CHECK: [[@LINE+2]]:25: error: expected relocation specifier after '%'
# CHECK: [[@LINE+1]]:25: error: failed to parse operand
        addi    r2, r1, %
## The other immediates take no %tprel.
# CHECK: [[@LINE+1]]:25: error: immediate must be an integer in the range [-2147483648, 2147483647]{{$}}
        addiw   r2, r1, %tprel(sym)
# CHECK: [[@LINE+1]]:25: error: immediate must be an integer in the range [0, 4294967295]{{$}}
        addv    r2, r1, %tprel(sym)
.else
## Symbol operands of other instructions: SETRU would zero-extend the
## negative offset, the rest are addresses or PC-relative targets.
# OBJ: [[@LINE+1]]:9: error: %tprel is not allowed in this operand
        setru   r1, %tprel(sym)
# OBJ: [[@LINE+1]]:9: error: %tprel is not allowed in this operand
        memreadr r1, %tprel(sym)
# OBJ: [[@LINE+1]]:9: error: %tprel is not allowed in this operand
        call    %tprel(sym)
# OBJ: [[@LINE+1]]:9: error: %tprel is not allowed in this operand
        callrel %tprel(sym)
# OBJ: [[@LINE+1]]:9: error: %tprel is not allowed in this operand
        leapc   r1, %tprel(sym)
## No data relocation.
# OBJ: [[@LINE+1]]:17: error: %tprel is only allowed as the immediate of addi, setr or an indexed load / store
        .word   %tprel(sym)
# OBJ: [[@LINE+1]]:17: error: %tprel is only allowed as the immediate of addi, setr or an indexed load / store
        .quad   %tprel(sym)
## Only the linker knows a TP offset.
# OBJ: [[@LINE+1]]:25: error: %tprel needs a TLS symbol, not a constant
        addi    r2, r1, %tprel(5)
## A symbol this object defines outside TLS.
# OBJ: [[@LINE+1]]:25: error: %tprel of 'plain', which is not in a TLS section
        addi    r2, r1, %tprel(plain)
        .data
plain:  .long 0
.endif
