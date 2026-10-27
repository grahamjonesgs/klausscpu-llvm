# RUN: llvm-mc -triple=klausscpu-unknown-elf -show-encoding %s | FileCheck %s
# RUN: llvm-mc -triple=klausscpu-unknown-elf -filetype=obj %s -o %t.o
# RUN: llvm-readobj -r %t.o | FileCheck %s --check-prefix=RELOC
# RUN: not llvm-mc -triple=klausscpu-unknown-elf --defsym=ERR=1 %s \
# RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR --implicit-check-not=error:

# Registers have no prefix, so a symbol may be spelled like one: r0-r15, the
# letter names a-p (any case), sp, ra, flags.  Such a name is the register
# where the instruction takes a register and the symbol where it takes an
# immediate or target, so compiler output for C names like `b` or `K`
# reassembles.

# Registers stay registers.
# CHECK: addr r1, r2, r3            # encoding: [0x23,0x01,0x20,0x44]
        addr    b, C, d
# CHECK: copy r15, r12              # encoding: [0xc0,0x0f,0x00,0x54]
        copy    p, m

# Targets and addresses are symbols.
# CHECK: callrel b                  # encoding: [A,A,0b000000AA,0x63]
# CHECK-NEXT:                       #   fixup A - offset: 0, value: b, kind: FK_KlaussCPU_CALL18
        call    b
# CHECK: callrel sp                 # encoding: [A,A,0b000000AA,0x63]
# CHECK-NEXT:                       #   fixup A - offset: 0, value: sp, kind: FK_KlaussCPU_CALL18
        callrel sp
# CHECK: setr r12, flags            # encoding: [0x00,0x0c,0xd0,0x8b,A,A,A,A]
# CHECK-NEXT:                       #   fixup A - offset: 4, value: flags, kind: FK_KlaussCPU_ABS32
        setr    r12, flags
# CHECK: leapc r9, K                # encoding: [0x00,0x09,0x80,0x8b,A,A,A,A]
# CHECK-NEXT:                       #   fixup A - offset: 4, value: K, kind: FK_KlaussCPU_PCREL32
        leapc   r9, K
# A register name that starts a longer expression is the symbol.
# CHECK: setr r1, ra+8              # encoding: [0x00,0x01,0xd0,0x8b,A,A,A,A]
# CHECK-NEXT:                       #   fixup A - offset: 4, value: ra+8, kind: FK_KlaussCPU_ABS32
        setr    r1, ra+8
# CHECK: memsetr r1, r7             # encoding: [0x00,0x01,0x40,0x9f,A,A,A,A]
# CHECK-NEXT:                       #   fixup A - offset: 4, value: r7, kind: FK_KlaussCPU_ABS32
        memsetr r1, r7
# CHECK: fbr r1, r2, 0, a           # encoding: [0x12,A,0b000AAAAA,0x74]
# CHECK-NEXT:                       #   fixup A - offset: 0, value: a, kind: FK_KlaussCPU_PCREL13
        fbr     r1, r2, 0, a

        .globl  b
a:
b:
        ret

# RELOC:      Section ({{.*}}) .rela.text {
# RELOC-NEXT:   0x8 R_KLAUSSCPU_CALL18 b 0x0
# RELOC-NEXT:   0xC R_KLAUSSCPU_CALL18 sp 0x0
# RELOC-NEXT:   0x14 R_KLAUSSCPU_ABS32 flags 0x0
# RELOC-NEXT:   0x1C R_KLAUSSCPU_PCREL32 K 0x0
# RELOC-NEXT:   0x24 R_KLAUSSCPU_ABS32 ra 0x8
# RELOC-NEXT:   0x2C R_KLAUSSCPU_ABS32 r7 0x0
# RELOC-NEXT: }

.ifdef ERR
# No fixup exists for these immediates, so the symbol reading is rejected.
# ERR: [[#@LINE+1]]:25: error: immediate must be an integer in the range [0, 4294967295]
        addv    r1, r2, b
# ERR: [[#@LINE+1]]:25: error: immediate must be an integer in the range [0, 63]
        shlv    r1, r2, sp
# A register operand still has to be a GPR.
# ERR: [[#@LINE+1]]:21: error: invalid operand for instruction
        copy    r1, sp
.endif
