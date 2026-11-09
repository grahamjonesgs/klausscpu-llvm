# RUN: llvm-mc -triple=klausscpu-unknown-elf -show-encoding %s \
# RUN:   | FileCheck %s
# RUN: llvm-mc -triple=klausscpu-unknown-elf -filetype=obj %s -o %t.o
# RUN: llvm-objdump -d --no-show-raw-insn %t.o | FileCheck %s --check-prefix=DIS
# RUN: not llvm-mc -triple=klausscpu-unknown-elf --defsym=ERR=1 %s \
# RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR --implicit-check-not=error:

# MMU stage 1 instructions (klaussCPU branch mmu; MMU_PLAN.md §5, §5.1,
# §6.1; ISA_ENCODING_V2_MAP.md §9).  Every encoding below was computed from
# the ISA map's templates and field positions, not from this backend.  The
# first block of each kind uses distinct registers so that every register
# field is pinned; the DIS lines check that the object disassembles back to
# the same text (SR names where they have one, numbers otherwise; the names
# of the stage 2/3 SRs are in mmu-stage23.s).

## SYSCALL = 0x6C080000
# CHECK: syscall                    # encoding: [0x00,0x00,0x08,0x6c]
# DIS: syscall
        syscall
# CHECK: syscall                    # encoding: [0x00,0x00,0x08,0x6c]
# DIS-NEXT: syscall
        SYSCALL

## MFSR rd, sr = 0x6C400000 | sr << 12 | rd << 8, every named SR
# CHECK: mfsr r1, status            # encoding: [0x00,0x01,0x40,0x6c]
# DIS-NEXT: mfsr r1, status
        mfsr    r1, status
# CHECK: mfsr r2, evec              # encoding: [0x00,0x12,0x40,0x6c]
# DIS-NEXT: mfsr r2, evec
        mfsr    r2, evec
# CHECK: mfsr r3, ecause            # encoding: [0x00,0x23,0x40,0x6c]
# DIS-NEXT: mfsr r3, ecause
        MFSR    r3, ECAUSE
# CHECK: mfsr r4, eaddr             # encoding: [0x00,0x34,0x40,0x6c]
# DIS-NEXT: mfsr r4, eaddr
        mfsr    r4, eaddr
# CHECK: mfsr r5, usp               # encoding: [0x00,0x45,0x40,0x6c]
# DIS-NEXT: mfsr r5, usp
        mfsr    r5, usp
# CHECK: mfsr r6, scratch0          # encoding: [0x00,0x56,0x40,0x6c]
# DIS-NEXT: mfsr r6, scratch0
        mfsr    r6, scratch0
# CHECK: mfsr r7, scratch1          # encoding: [0x00,0x67,0x40,0x6c]
# DIS-NEXT: mfsr r7, scratch1
        mfsr    r7, Scratch1
# CHECK: mfsr r2, tp                # encoding: [0x00,0x72,0x40,0x6c]
# DIS-NEXT: mfsr r2, tp
        mfsr    r2, tp
# CHECK: mfsr r15, cycle            # encoding: [0x00,0x8f,0x40,0x6c]
# DIS-NEXT: mfsr r15, cycle
        mfsr    r15, cycle

## Numeric SRs: a named one prints as its name, others as numbers
# CHECK: mfsr r0, cycle             # encoding: [0x00,0x80,0x40,0x6c]
# DIS-NEXT: mfsr r0, cycle
        mfsr    r0, 8
# CHECK: mfsr r1, 9                 # encoding: [0x00,0x91,0x40,0x6c]
# DIS-NEXT: mfsr r1, 9
        mfsr    r1, 9
# CHECK: mfsr r1, mpu_base0         # encoding: [0x00,0x01,0x41,0x6c]
# DIS-NEXT: mfsr r1, mpu_base0
        mfsr    r1, 0x10
# CHECK: mfsr r12, 255              # encoding: [0x00,0xfc,0x4f,0x6c]
# DIS-NEXT: mfsr r12, 255
        mfsr    r12, 255
.set SR_MPU0, 0x10
# CHECK: mfsr r1, mpu_base0         # encoding: [0x00,0x01,0x41,0x6c]
# DIS-NEXT: mfsr r1, mpu_base0
        mfsr    r1, SR_MPU0

## MTSR sr, rs1 = 0x6C600000 | sr << 12 | rs1 << 4
# CHECK: mtsr evec, r1              # encoding: [0x10,0x10,0x60,0x6c]
# DIS-NEXT: mtsr evec, r1
        MTSR    EVEC, r1
# CHECK: mtsr status, r9            # encoding: [0x90,0x00,0x60,0x6c]
# DIS-NEXT: mtsr status, r9
        mtsr    status, r9
# CHECK: mtsr tp, r15               # encoding: [0xf0,0x70,0x60,0x6c]
# DIS-NEXT: mtsr tp, r15
        mtsr    tp, r15
# CHECK: mtsr scratch1, r0          # encoding: [0x00,0x60,0x60,0x6c]
# DIS-NEXT: mtsr scratch1, r0
        mtsr    6, r0
# CHECK: mtsr 63, r4                # encoding: [0x40,0xf0,0x63,0x6c]
# DIS-NEXT: mtsr 63, r4
        mtsr    63, r4
# CHECK: mtsr 255, r2               # encoding: [0x20,0xf0,0x6f,0x6c]
# DIS-NEXT: mtsr 255, r2
        mtsr    255, r2

## LR rd, rs1 (address): class 6, MODE=00, [12] = 1; rd [11:8], rs1 [7:4]
# CHECK: lr64 r3, r1                # encoding: [0x10,0x13,0x00,0x5b]
# DIS-NEXT: lr64 r3, r1
        lr64    r3, r1
# CHECK: lr32 r10, r14              # encoding: [0xe0,0x1a,0x00,0x5a]
# DIS-NEXT: lr32 r10, r14
        lr32    r10, r14
# CHECK: lr32s r5, r6               # encoding: [0x60,0x15,0x80,0x5a]
# DIS-NEXT: lr32s r5, r6
        lr32s   r5, r6
## The `_s` spelling, as ldidx32_s, and the ISA map's symbols.
# CHECK: lr32s r5, r6               # encoding: [0x60,0x15,0x80,0x5a]
# DIS-NEXT: lr32s r5, r6
        lr32_s  r5, r6
# CHECK: lr64 r3, r1                # encoding: [0x10,0x13,0x00,0x5b]
# DIS-NEXT: lr64 r3, r1
        LR_D    r3, r1
# CHECK: lr32 r3, r1                # encoding: [0x10,0x13,0x00,0x5a]
# DIS-NEXT: lr32 r3, r1
        LR_W    r3, r1
# CHECK: lr32s r3, r1               # encoding: [0x10,0x13,0x80,0x5a]
# DIS-NEXT: lr32s r3, r1
        LR_W_S  r3, r1

## SC rd (result), rs2 (data), rs1 (address): class 7, MODE=00, [12] = 1;
## rd [11:8], rs1 [7:4], rs2 [3:0].  The ISA map's example first.
# CHECK: sc64 r6, r5, r1            # encoding: [0x15,0x16,0x00,0x5f]
# DIS-NEXT: sc64 r6, r5, r1
        sc64    r6, r5, r1
# CHECK: sc32 r6, r5, r1            # encoding: [0x15,0x16,0x00,0x5e]
# DIS-NEXT: sc32 r6, r5, r1
        sc32    r6, r5, r1
# CHECK: sc64 r12, r2, r9           # encoding: [0x92,0x1c,0x00,0x5f]
# DIS-NEXT: sc64 r12, r2, r9
        sc64    r12, r2, r9
# CHECK: sc32 r0, r15, r7           # encoding: [0x7f,0x10,0x00,0x5e]
# DIS-NEXT: sc32 r0, r15, r7
        sc32    r0, r15, r7
# CHECK: sc64 r6, r5, r1            # encoding: [0x15,0x16,0x00,0x5f]
# DIS-NEXT: sc64 r6, r5, r1
        SC_D    r6, r5, r1
# CHECK: sc32 r6, r5, r1            # encoding: [0x15,0x16,0x00,0x5e]
# DIS-NEXT: sc32 r6, r5, r1
        SC_W    r6, r5, r1

.ifdef ERR
# ERR: [[#@LINE+1]]:21: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec or an integer in the range [0, 255]
        mfsr    r1, sr9
# ERR: [[#@LINE+1]]:21: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec or an integer in the range [0, 255]
        mfsr    r1, 256
# ERR: [[#@LINE+1]]:21: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec or an integer in the range [0, 255]
        mfsr    r1, -1
# ERR: [[#@LINE+1]]:21: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec or an integer in the range [0, 255]
        mfsr    r1, r2
# ERR: [[#@LINE+1]]:21: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec or an integer in the range [0, 255]
        mfsr    r1, tp+1
# ERR: [[#@LINE+1]]:17: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec or an integer in the range [0, 255]
        mtsr    sp_alt, r1
# ERR: [[#@LINE+1]]:17: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec or an integer in the range [0, 255]
        mtsr    0x100, r1
# ERR: [[#@LINE+1]]:17: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec or an integer in the range [0, 255]
        mtsr    r1, tp
# ERR: [[#@LINE+1]]:17: error: invalid operand for instruction
        mfsr    tp, r1

## Wrong operand counts.
# ERR: [[#@LINE+1]]:17: error: invalid operand for instruction
        syscall r1
# ERR: [[#@LINE+1]]:9: error: too few operands for instruction
        mfsr    r1
# ERR: [[#@LINE+1]]:25: error: invalid operand for instruction
        mfsr    r1, tp, r2
# ERR: [[#@LINE+1]]:9: error: too few operands for instruction
        mtsr    tp
# ERR: [[#@LINE+1]]:25: error: invalid operand for instruction
        mtsr    tp, r1, r2
# ERR: [[#@LINE+1]]:9: error: too few operands for instruction
        lr64    r1
# ERR: [[#@LINE+1]]:25: error: invalid operand for instruction
        lr32s   r1, r2, r3
# ERR: [[#@LINE+1]]:9: error: too few operands for instruction
        sc64    r1, r2
# ERR: [[#@LINE+1]]:29: error: invalid operand for instruction
        sc32    r1, r2, r3, r4
.endif
