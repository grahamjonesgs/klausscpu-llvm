# RUN: llvm-mc -triple=klausscpu-unknown-elf -show-encoding %s \
# RUN:   | FileCheck %s --check-prefixes=CHECK,SHORT
# RUN: llvm-mc -triple=klausscpu-unknown-elf -show-encoding \
# RUN:   -klausscpu-short-forms=false %s | FileCheck %s --check-prefixes=CHECK,LONG

# Every operand kind whose EncoderMethod emits a fixup for a symbolic value
# (KlaussCPUInstrInfo.td operand types, KlaussCPUMCCodeEmitter): the field is
# zero and the fixup has the right kind and byte offset.  ABS32/PCREL32 cover
# word 1 (offset 4); the short-branch PCREL18 and fused-branch PCREL13 fields
# are in word 0 (offset 0).  A symbol keeps setr/memreadr/memsetr long.

## simm32_abs (SETR, MEMREADR, MEMSETR) -> FK_KlaussCPU_ABS32 at 4
# CHECK:      setr r1, sym # encoding: [0x00,0x01,0xd0,0x8b,A,A,A,A]
# CHECK-NEXT: fixup A - offset: 4, value: sym, kind: FK_KlaussCPU_ABS32
        setr     r1, sym
# CHECK:      memreadr r2, sym # encoding: [0x00,0x02,0x40,0x9b,A,A,A,A]
# CHECK-NEXT: fixup A - offset: 4, value: sym, kind: FK_KlaussCPU_ABS32
        memreadr r2, sym
# CHECK:      memsetr r3, sym+8 # encoding: [0x00,0x03,0x40,0x9f,A,A,A,A]
# CHECK-NEXT: fixup A - offset: 4, value: sym+8, kind: FK_KlaussCPU_ABS32
        memsetr  r3, sym+8

## CallTarget32 -> FK_KlaussCPU_ABS32 at 4 (calls are never shortened)
# CHECK:      call sym # encoding: [0x00,0x00,0x00,0xa2,A,A,A,A]
# CHECK-NEXT: fixup A - offset: 4, value: sym, kind: FK_KlaussCPU_ABS32
        call     sym
# CHECK:      callne sym # encoding: [0x00,0x00,0x4c,0xa2,A,A,A,A]
# CHECK-NEXT: fixup A - offset: 4, value: sym, kind: FK_KlaussCPU_ABS32
        callne   sym

## PCRelCallTarget32 / pcrel_simm32 -> FK_KlaussCPU_PCREL32 at 4
# CHECK:      callrel sym # encoding: [0x00,0x00,0x00,0xa3,A,A,A,A]
# CHECK-NEXT: fixup A - offset: 4, value: sym, kind: FK_KlaussCPU_PCREL32
        callrel  sym
# CHECK:      leapc r4, sym # encoding: [0x00,0x04,0x80,0x8b,A,A,A,A]
# CHECK-NEXT: fixup A - offset: 4, value: sym, kind: FK_KlaussCPU_PCREL32
        leapc    r4, sym

## Branches to a symbol start as the short form (PCRelTarget18,
## FK_KlaussCPU_PCREL18 at 0); without short forms they keep BrTarget32
## (ABS32) or PCRelTarget32 (PCREL32) in word 1.
# SHORT:      jmprel sym # encoding: [A,A,0b000000AA,0x61]
# SHORT-NEXT: fixup A - offset: 0, value: sym, kind: FK_KlaussCPU_PCREL18
# LONG:       jmp sym # encoding: [0x00,0x00,0x00,0xa0,A,A,A,A]
# LONG-NEXT:  fixup A - offset: 4, value: sym, kind: FK_KlaussCPU_ABS32
        jmp      sym
# SHORT:      jmpzrel sym # encoding: [A,A,0b000010AA,0x61]
# SHORT-NEXT: fixup A - offset: 0, value: sym, kind: FK_KlaussCPU_PCREL18
# LONG:       jmpz sym # encoding: [0x00,0x00,0x08,0xa0,A,A,A,A]
# LONG-NEXT:  fixup A - offset: 4, value: sym, kind: FK_KlaussCPU_ABS32
        jmpz     sym
# SHORT:      jmpno sym # encoding: [A,A,0b000111AA,0x61]
# SHORT-NEXT: fixup A - offset: 0, value: sym, kind: FK_KlaussCPU_PCREL18
# LONG:       jmpno sym # encoding: [0x00,0x00,0x1c,0xa0,A,A,A,A]
# LONG-NEXT:  fixup A - offset: 4, value: sym, kind: FK_KlaussCPU_ABS32
        jmpno    sym
# SHORT:      jmpnerel sym # encoding: [A,A,0b010011AA,0x61]
# SHORT-NEXT: fixup A - offset: 0, value: sym, kind: FK_KlaussCPU_PCREL18
# LONG:       jmpnerel sym # encoding: [0x00,0x00,0x4c,0xa1,A,A,A,A]
# LONG-NEXT:  fixup A - offset: 4, value: sym, kind: FK_KlaussCPU_PCREL32
        jmpnerel sym

## PCRelTarget13 -> FK_KlaussCPU_PCREL13 at 0
# CHECK:      fbr r1, r2, 9, sym # encoding: [0x12,A,0b110AAAAA,0x74]
# CHECK-NEXT: fixup A - offset: 0, value: sym, kind: FK_KlaussCPU_PCREL13
        fbr      r1, r2, 9, sym
# CHECK:      fbri r3, -2, 4, sym # encoding: [0x3e,A,0b001AAAAA,0x76]
# CHECK-NEXT: fixup A - offset: 0, value: sym, kind: FK_KlaussCPU_PCREL13
        fbri     r3, -2, 4, sym
