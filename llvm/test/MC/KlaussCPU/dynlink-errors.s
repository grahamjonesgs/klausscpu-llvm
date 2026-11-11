# RUN: not llvm-mc -triple=klausscpu-unknown-linux-musl %s 2>&1 \
# RUN:   | FileCheck %s --implicit-check-not=error:
# RUN: not llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj \
# RUN:   --defsym=OBJ=1 %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=OBJ --implicit-check-not=error:

# Where the dynamic-linking specifiers are allowed (ABI.md): %gotpcrel,
# %gottprel and %tlsgd as the offset of leapc, %plt as the target of
# callrel, jmprel and tail, each around a bare symbol (an offset is added
# after the access, so the relocation's addend is 0).

.ifndef OBJ
## An offset inside or after the specifier.
# CHECK: [[@LINE+2]]:31: error: %gotpcrel takes a symbol without an offset (add the offset after the access)
# CHECK: [[@LINE+1]]:35: error: failed to parse operand
        leapc   r1, %gotpcrel(x+8)
## The generic parser reads an offset after the parenthesis into it.
# CHECK: [[@LINE+2]]:31: error: %gotpcrel takes a symbol without an offset (add the offset after the access)
# CHECK: [[@LINE+1]]:35: error: failed to parse operand
        leapc   r1, %gotpcrel(x)+8
# CHECK: [[@LINE+1]]:17: error: %plt takes a symbol without an offset (add the offset after the access)
        callrel 4+%plt(f)
# CHECK: [[@LINE+2]]:31: error: %gotpcrel takes a symbol without an offset (add the offset after the access)
# CHECK: [[@LINE+1]]:33: error: failed to parse operand
        leapc   r1, %gotpcrel(5)
# CHECK: [[@LINE+2]]:28: error: expected '(' after %tlsgd
# CHECK: [[@LINE+1]]:28: error: failed to parse operand
        leapc   r1, %tlsgd x
## Mismatched specifier and instruction.
# CHECK: [[@LINE+1]]:21: error: %plt is only allowed as the target of callrel, jmprel or tail
        leapc   r1, %plt(f)
# CHECK: [[@LINE+1]]:17: error: %gotpcrel is only allowed as the offset of leapc
        callrel %gotpcrel(f)
# CHECK: [[@LINE+1]]:14: error: %tlsgd is only allowed as the offset of leapc
        tail %tlsgd(f)
## The absolute call and the conditional branches have no PLT form.
# CHECK: [[@LINE+1]]:17: error: %plt is only allowed as the target of callrel, jmprel or tail
        call    %plt(f)
# CHECK: [[@LINE+1]]:17: error: %plt is only allowed as the target of callrel, jmprel or tail
        jmpzrel %plt(f)
# CHECK: [[@LINE+1]]:17: error: %plt is only allowed as the target of callrel, jmprel or tail
        jmp     %plt(f)
## Other symbolic operands.
# CHECK: [[@LINE+1]]:21: error: %gotpcrel is only allowed as the offset of leapc
        setr    r1, %gotpcrel(x)
# CHECK: [[@LINE+1]]:21: error: %gottprel is only allowed as the offset of leapc
        setru   r1, %gottprel(x)
# CHECK: [[@LINE+1]]:25: error: immediate must be an integer in the range [-2147483648, 2147483647] or %tprel(symbol)
        addi    r1, r1, %tlsgd(x)
# CHECK: [[@LINE+1]]:25: error: immediate must be an integer in the range [-2147483648, 2147483647] or %tprel(symbol)
        ldidx64 r1, r1, %gottprel(x)
# CHECK: [[@LINE+1]]:25: error: immediate must be an integer in the range [-2147483648, 2147483647] or %tprel(symbol)
        stidx64 r1, r2, %gotpcrel(x)
.else
## Data directives have no such relocation.
# OBJ: [[@LINE+1]]:17: error: %gotpcrel is only allowed as the offset of a 2-word leapc
        .quad   %gotpcrel(x)
# OBJ: [[@LINE+1]]:17: error: %plt is only allowed as the target of a 2-word callrel, jmprel or tail
        .long   %plt(f)
# OBJ: [[@LINE+1]]:17: error: %tlsgd is only allowed as the offset of a 2-word leapc
        .word   %tlsgd(tv)
## An offset or a constant that only shows once the expression is
## evaluated.
# OBJ: [[@LINE+1]]:21: error: %gotpcrel takes a symbol without an offset
        leapc   r1, %gotpcrel(y)
        .set    y, x+4
# OBJ: [[@LINE+1]]:17: error: %plt needs a symbol
        callrel %plt(c)
        .set    c, 12
## A symbol this object defines outside TLS.
# OBJ: [[@LINE+1]]:21: error: %gottprel of 'plain', which is not in a TLS section
        leapc   r1, %gottprel(plain)
# OBJ: [[@LINE+1]]:21: error: %tlsgd of 'plain', which is not in a TLS section
        leapc   r1, %tlsgd(plain)
        .data
plain:  .long 0
.endif
