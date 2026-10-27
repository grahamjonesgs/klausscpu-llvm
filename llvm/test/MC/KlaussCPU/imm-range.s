# RUN: llvm-mc -triple=klausscpu-unknown-elf -show-encoding \
# RUN:   -klausscpu-short-forms=false %s | FileCheck %s
# RUN: not llvm-mc -triple=klausscpu-unknown-elf --defsym=ERR=1 %s \
# RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR --implicit-check-not=error:

# Every immediate operand class (KCImmAsmOperand in KlaussCPUInstrInfo.td)
# accepts exactly the values its field holds, and rejects anything else with
# the class's range instead of truncating it.  The first half pins the
# boundary values, the ERR half the first value outside each range.

# SImm32: imm32 sign-extended by the CPU (ADDI, CMPRV, MULV/DIVV/MODV, ADDSP,
# load/store offsets).
# CHECK: addi r1, r2, -2147483648   # encoding: [0x20,0x01,0x30,0x88,0x00,0x00,0x00,0x80]
        addi    r1, r2, -2147483648
# CHECK: cmprv r1, 2147483647       # encoding: [0x10,0x00,0x10,0x8c,0xff,0xff,0xff,0x7f]
        cmprv   r1, 2147483647
# CHECK: ldidx64 r1, r2, -2147483648 # encoding: [0x20,0x01,0x30,0x9b,0x00,0x00,0x00,0x80]
        ldidx64 r1, r2, -2147483648
# CHECK: addsp 2147483647           # encoding: [0x00,0x00,0x40,0xa5,0xff,0xff,0xff,0x7f]
        addsp   2147483647

# UImm32: imm32 zero-extended by the CPU (ADDV/MINUSV/ANDV/ORV/XORV, PUSHV)
# and the BEXTR/BDEP field descriptor.
# CHECK: andv r1, r2, 4294967295    # encoding: [0x20,0x01,0x00,0x89,0xff,0xff,0xff,0xff]
        andv    r1, r2, 0xFFFFFFFF
# CHECK: addv r1, r1, 0             # encoding: [0x10,0x01,0x20,0x88,0x00,0x00,0x00,0x00]
        addv    r1, 0
# CHECK: pushv 4294967295           # encoding: [0x00,0x00,0x40,0xa4,0xff,0xff,0xff,0xff]
        pushv   0xFFFFFFFF

# Imm32: a raw 32-bit word (SETR64 / PUSHV64 halves, peripheral immediates),
# in either spelling.
# CHECK: setr64 r1, -2147483648, 4294967295 # encoding: [0x00,0x01,0xc0,0xcb,0x00,0x00,0x00,0x80,0xff,0xff,0xff,0xff]
        setr64  r1, -2147483648, 0xFFFFFFFF
# CHECK: delayv 4294967295          # encoding: [0x00,0x00,0x05,0xac,0xff,0xff,0xff,0xff]
        delayv  0xFFFFFFFF

# Imm32Sym: SETR also takes the unsigned spelling of its 32-bit pattern
# (sign-extended like the ABS32 address of a symbol) and a symbol.
# CHECK: setr r0, 4027514880        # encoding: [0x00,0x00,0xd0,0x8b,0x00,0x00,0x0f,0xf0]
        setr    r0, 0xF00F0000
# CHECK: setr r0, -2147483648       # encoding: [0x00,0x00,0xd0,0x8b,0x00,0x00,0x00,0x80]
        setr    r0, -2147483648
# CHECK: setr r0, sym               # encoding: [0x00,0x00,0xd0,0x8b,A,A,A,A]
        setr    r0, sym

# UImm32Sym: absolute addresses (MEMREADR/MEMSETR, LDABSn/STABSn, JMP/CALL
# targets).
# CHECK: memreadr r1, 4294967295    # encoding: [0x00,0x01,0x40,0x9b,0xff,0xff,0xff,0xff]
        memreadr r1, 0xFFFFFFFF
# CHECK: stabs32 r1, 4294967292     # encoding: [0x00,0x01,0x40,0x9e,0xfc,0xff,0xff,0xff]
        stabs32 r1, 0xFFFFFFFC
# CHECK: call 0                     # encoding: [0x00,0x00,0x00,0xa2,0x00,0x00,0x00,0x00]
        call    0
# CHECK: jmp sym                    # encoding: [0x00,0x00,0x00,0xa0,A,A,A,A]
        jmp     sym

# PCRel32: a byte displacement or a symbol.
# CHECK: jmprel -2147483648         # encoding: [0x00,0x00,0x00,0xa1,0x00,0x00,0x00,0x80]
        jmprel  -2147483648
# CHECK: leapc r1, 2147483647       # encoding: [0x00,0x01,0x80,0x8b,0xff,0xff,0xff,0x7f]
        leapc   r1, 2147483647

# UImm6: shift count / bit index.
# CHECK: shlv r1, r2, 63            # encoding: [0x20,0xc1,0x3f,0x50]
        shlv    r1, r2, 63
# CHECK: btst r1, 0                 # encoding: [0x10,0x00,0xe0,0x52]
        btst    r1, 0

# UImm22: ENTER frame size in dwords.
# CHECK: enter 4194303              # encoding: [0xff,0xff,0x3f,0x66]
        enter   4194303

# SImm4 and the fused-branch displacement (a multiple of 4 in 13 words).
# CHECK: fbri r1, -8, 0, -16384     # encoding: [0x18,0x00,0x30,0x74]
        fbri    r1, -8, 0, -16384
# CHECK: fbr r1, r2, 0, 16380       # encoding: [0x12,0xff,0x0f,0x74]
        fbr     r1, r2, 0, 16380

.ifdef ERR
# ERR: [[#@LINE+1]]:25: error: immediate must be an integer in the range [-2147483648, 2147483647]
        addi    r1, r2, 0x80000000
# ERR: [[#@LINE+1]]:21: error: immediate must be an integer in the range [-2147483648, 2147483647]
        cmprv   r1, 0xFFFFFFFF
# ERR: [[#@LINE+1]]:25: error: immediate must be an integer in the range [-2147483648, 2147483647]
        ldidx64 r1, r2, 0x100000000
# ERR: [[#@LINE+1]]:25: error: immediate must be an integer in the range [-2147483648, 2147483647]
        mulv    r1, r2, -2147483649
# ERR: [[#@LINE+1]]:17: error: immediate must be an integer in the range [-2147483648, 2147483647]
        addsp   0x80000000
# ERR: [[#@LINE+1]]:25: error: immediate must be an integer in the range [0, 4294967295]
        addv    r1, r2, -1
# ERR: [[#@LINE+1]]:21: error: immediate must be an integer in the range [0, 4294967295]
        andv    r1, 0x100000000
# ERR: [[#@LINE+1]]:17: error: immediate must be an integer in the range [0, 4294967295]
        pushv   -5
# ERR: [[#@LINE+1]]:25: error: immediate must be an integer in the range [0, 4294967295]
        bextr   r1, r2, -1
# ERR: [[#@LINE+1]]:21: error: immediate must be an integer in the range [-2147483648, 4294967295]
        setr64  r1, 0x100000000, 0
# ERR: [[#@LINE+1]]:20: error: immediate must be an integer in the range [-2147483648, 4294967295]
        pushv64 0, -2147483649
# ERR: [[#@LINE+1]]:17: error: immediate must be an integer in the range [-2147483648, 4294967295]
        delayv  0x100000000
# ERR: [[#@LINE+1]]:21: error: immediate must be an integer in the range [-2147483648, 4294967295] or a symbol
        setr    r1, 0x100000005
# ERR: [[#@LINE+1]]:21: error: immediate must be an integer in the range [-2147483648, 4294967295] or a symbol
        setr    r1, -2147483649
# ERR: [[#@LINE+1]]:22: error: immediate must be an integer in the range [0, 4294967295] or a symbol
        memreadr r1, -4
# ERR: [[#@LINE+1]]:22: error: immediate must be an integer in the range [0, 4294967295] or a symbol
        ldabs8   r1, 0x100000000
# ERR: [[#@LINE+1]]:17: error: immediate must be an integer in the range [0, 4294967295] or a symbol
        call    0x100000000
# ERR: [[#@LINE+1]]:17: error: immediate must be an integer in the range [-2147483648, 2147483647] or a symbol
        jmprel  0x80000000
# ERR: [[#@LINE+1]]:21: error: immediate must be an integer in the range [-2147483648, 2147483647] or a symbol
        leapc   r1, -2147483649
# ERR: [[#@LINE+1]]:25: error: immediate must be an integer in the range [0, 63]
        shlv    r1, r2, 64
# ERR: [[#@LINE+1]]:25: error: immediate must be an integer in the range [0, 63]
        bset    r1, r2, -1
# ERR: [[#@LINE+1]]:21: error: immediate must be an integer in the range [0, 63]
        shrv    r1, 64
# ERR: [[#@LINE+1]]:21: error: immediate must be an integer in the range [0, 63]
        btst    r1, 64
# ERR: [[#@LINE+1]]:17: error: immediate must be an integer in the range [0, 4194303]
        enter   4194304
# ERR: [[#@LINE+1]]:17: error: immediate must be an integer in the range [0, 4194303]
        enter   -1
# ERR: [[#@LINE+1]]:21: error: immediate must be an integer in the range [-8, 7]
        fbri    r1, -9, 0, lab
# ERR: [[#@LINE+1]]:28: error: immediate must be a multiple of 4 in the range [-16384, 16380] or a symbol
        fbr     r1, r2, 0, 16384
# ERR: [[#@LINE+1]]:28: error: immediate must be a multiple of 4 in the range [-16384, 16380] or a symbol
        fbr     r1, r2, 0, 6
# A register where an immediate is expected gets the class's message too.
# ERR: [[#@LINE+1]]:25: error: immediate must be an integer in the range [0, 4294967295]
        addv    r1, r2, r3
# A missing operand is reported as such, not as the range of the class the
# closest form expected there.
# ERR: [[#@LINE+1]]:9: error: too few operands for instruction
        addv    r1, r2
# ERR: [[#@LINE+1]]:9: error: too few operands for instruction
        bextr   r1, r2
# ERR: [[#@LINE+1]]:9: error: too few operands for instruction
        shlv    r1, r2
# ERR: [[#@LINE+1]]:9: error: too few operands for instruction
        setr    r1
lab:
.endif
