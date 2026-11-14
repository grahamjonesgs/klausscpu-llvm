# RUN: llvm-mc -triple=klausscpu-unknown-elf -show-encoding %s \
# RUN:   | FileCheck %s
# RUN: llvm-mc -triple=klausscpu-unknown-elf -filetype=obj %s -o %t.o
# RUN: llvm-objdump -d --no-show-raw-insn %t.o | FileCheck %s --check-prefix=DIS
# RUN: not llvm-mc -triple=klausscpu-unknown-elf --defsym=ERR=1 %s \
# RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR --implicit-check-not=error:

# MMU stages 2 and 3 (klaussCPU branch mmu; MMU_PLAN.md §10.1, §11.3;
# ISA_ENCODING_V2_MAP.md §10): the MPU and TLB system registers of MFSR /
# MTSR and the four TLB instructions.  Every encoding below was computed
# from the ISA map's templates and field positions, not from this backend.
# The DIS lines check that the object disassembles back to the same text.

## TLB instructions (class B, no operands)

# CHECK: tlbwi                      # encoding: [0x00,0x00,0x09,0x6c]
# DIS: tlbwi
        tlbwi
# CHECK: tlbwi                      # encoding: [0x00,0x00,0x09,0x6c]
# DIS-NEXT: tlbwi
        TLBWI
# CHECK: tlbr                       # encoding: [0x00,0x00,0x0a,0x6c]
# DIS-NEXT: tlbr
        tlbr
# CHECK: tlbr                       # encoding: [0x00,0x00,0x0a,0x6c]
# DIS-NEXT: tlbr
        TLBR
# CHECK: tlbp                       # encoding: [0x00,0x00,0x0b,0x6c]
# DIS-NEXT: tlbp
        tlbp
# CHECK: tlbp                       # encoding: [0x00,0x00,0x0b,0x6c]
# DIS-NEXT: tlbp
        TLBP
# CHECK: tlbinv                     # encoding: [0x00,0x00,0x0c,0x6c]
# DIS-NEXT: tlbinv
        tlbinv
# CHECK: tlbinv                     # encoding: [0x00,0x00,0x0c,0x6c]
# DIS-NEXT: tlbinv
        TLBINV

## MFSR rd, sr = 0x6C400000 | sr << 12 | rd << 8: every stage 2/3 SR name,
## in lower, upper or mixed case; MPU_BASEn = 0x10 + 2n, MPU_LIMITn = 0x11 + 2n
# CHECK: mfsr r1, mpu_base0         # encoding: [0x00,0x01,0x41,0x6c]
# DIS-NEXT: mfsr r1, mpu_base0
        mfsr    r1, mpu_base0
# CHECK: mfsr r8, mpu_limit0        # encoding: [0x00,0x18,0x41,0x6c]
# DIS-NEXT: mfsr r8, mpu_limit0
        mfsr    r8, MPU_LIMIT0
# CHECK: mfsr r15, mpu_base1        # encoding: [0x00,0x2f,0x41,0x6c]
# DIS-NEXT: mfsr r15, mpu_base1
        mfsr    r15, Mpu_base1
# CHECK: mfsr r6, mpu_limit1        # encoding: [0x00,0x36,0x41,0x6c]
# DIS-NEXT: mfsr r6, mpu_limit1
        mfsr    r6, mpu_limit1
# CHECK: mfsr r13, mpu_base2        # encoding: [0x00,0x4d,0x41,0x6c]
# DIS-NEXT: mfsr r13, mpu_base2
        mfsr    r13, MPU_BASE2
# CHECK: mfsr r4, mpu_limit2        # encoding: [0x00,0x54,0x41,0x6c]
# DIS-NEXT: mfsr r4, mpu_limit2
        mfsr    r4, Mpu_limit2
# CHECK: mfsr r11, mpu_base3        # encoding: [0x00,0x6b,0x41,0x6c]
# DIS-NEXT: mfsr r11, mpu_base3
        mfsr    r11, mpu_base3
# CHECK: mfsr r2, mpu_limit3        # encoding: [0x00,0x72,0x41,0x6c]
# DIS-NEXT: mfsr r2, mpu_limit3
        mfsr    r2, MPU_LIMIT3
# CHECK: mfsr r9, mpu_base4         # encoding: [0x00,0x89,0x41,0x6c]
# DIS-NEXT: mfsr r9, mpu_base4
        mfsr    r9, Mpu_base4
# CHECK: mfsr r0, mpu_limit4        # encoding: [0x00,0x90,0x41,0x6c]
# DIS-NEXT: mfsr r0, mpu_limit4
        mfsr    r0, mpu_limit4
# CHECK: mfsr r7, mpu_base5         # encoding: [0x00,0xa7,0x41,0x6c]
# DIS-NEXT: mfsr r7, mpu_base5
        mfsr    r7, MPU_BASE5
# CHECK: mfsr r14, mpu_limit5       # encoding: [0x00,0xbe,0x41,0x6c]
# DIS-NEXT: mfsr r14, mpu_limit5
        mfsr    r14, Mpu_limit5
# CHECK: mfsr r5, mpu_base6         # encoding: [0x00,0xc5,0x41,0x6c]
# DIS-NEXT: mfsr r5, mpu_base6
        mfsr    r5, mpu_base6
# CHECK: mfsr r12, mpu_limit6       # encoding: [0x00,0xdc,0x41,0x6c]
# DIS-NEXT: mfsr r12, mpu_limit6
        mfsr    r12, MPU_LIMIT6
# CHECK: mfsr r3, mpu_base7         # encoding: [0x00,0xe3,0x41,0x6c]
# DIS-NEXT: mfsr r3, mpu_base7
        mfsr    r3, Mpu_base7
# CHECK: mfsr r10, mpu_limit7       # encoding: [0x00,0xfa,0x41,0x6c]
# DIS-NEXT: mfsr r10, mpu_limit7
        mfsr    r10, mpu_limit7
# CHECK: mfsr r1, mpu_base8         # encoding: [0x00,0x01,0x42,0x6c]
# DIS-NEXT: mfsr r1, mpu_base8
        mfsr    r1, MPU_BASE8
# CHECK: mfsr r8, mpu_limit8        # encoding: [0x00,0x18,0x42,0x6c]
# DIS-NEXT: mfsr r8, mpu_limit8
        mfsr    r8, Mpu_limit8
# CHECK: mfsr r15, mpu_base9        # encoding: [0x00,0x2f,0x42,0x6c]
# DIS-NEXT: mfsr r15, mpu_base9
        mfsr    r15, mpu_base9
# CHECK: mfsr r6, mpu_limit9        # encoding: [0x00,0x36,0x42,0x6c]
# DIS-NEXT: mfsr r6, mpu_limit9
        mfsr    r6, MPU_LIMIT9
# CHECK: mfsr r13, mpu_base10       # encoding: [0x00,0x4d,0x42,0x6c]
# DIS-NEXT: mfsr r13, mpu_base10
        mfsr    r13, Mpu_base10
# CHECK: mfsr r4, mpu_limit10       # encoding: [0x00,0x54,0x42,0x6c]
# DIS-NEXT: mfsr r4, mpu_limit10
        mfsr    r4, mpu_limit10
# CHECK: mfsr r11, mpu_base11       # encoding: [0x00,0x6b,0x42,0x6c]
# DIS-NEXT: mfsr r11, mpu_base11
        mfsr    r11, MPU_BASE11
# CHECK: mfsr r2, mpu_limit11       # encoding: [0x00,0x72,0x42,0x6c]
# DIS-NEXT: mfsr r2, mpu_limit11
        mfsr    r2, Mpu_limit11
# CHECK: mfsr r9, mpu_base12        # encoding: [0x00,0x89,0x42,0x6c]
# DIS-NEXT: mfsr r9, mpu_base12
        mfsr    r9, mpu_base12
# CHECK: mfsr r0, mpu_limit12       # encoding: [0x00,0x90,0x42,0x6c]
# DIS-NEXT: mfsr r0, mpu_limit12
        mfsr    r0, MPU_LIMIT12
# CHECK: mfsr r7, mpu_base13        # encoding: [0x00,0xa7,0x42,0x6c]
# DIS-NEXT: mfsr r7, mpu_base13
        mfsr    r7, Mpu_base13
# CHECK: mfsr r14, mpu_limit13      # encoding: [0x00,0xbe,0x42,0x6c]
# DIS-NEXT: mfsr r14, mpu_limit13
        mfsr    r14, mpu_limit13
# CHECK: mfsr r5, mpu_base14        # encoding: [0x00,0xc5,0x42,0x6c]
# DIS-NEXT: mfsr r5, mpu_base14
        mfsr    r5, MPU_BASE14
# CHECK: mfsr r12, mpu_limit14      # encoding: [0x00,0xdc,0x42,0x6c]
# DIS-NEXT: mfsr r12, mpu_limit14
        mfsr    r12, Mpu_limit14
# CHECK: mfsr r3, mpu_base15        # encoding: [0x00,0xe3,0x42,0x6c]
# DIS-NEXT: mfsr r3, mpu_base15
        mfsr    r3, mpu_base15
# CHECK: mfsr r10, mpu_limit15      # encoding: [0x00,0xfa,0x42,0x6c]
# DIS-NEXT: mfsr r10, mpu_limit15
        mfsr    r10, MPU_LIMIT15
# CHECK: mfsr r1, tlb_hi            # encoding: [0x00,0x01,0x43,0x6c]
# DIS-NEXT: mfsr r1, tlb_hi
        mfsr    r1, Tlb_hi
# CHECK: mfsr r8, tlb_lo            # encoding: [0x00,0x18,0x43,0x6c]
# DIS-NEXT: mfsr r8, tlb_lo
        mfsr    r8, tlb_lo
# CHECK: mfsr r15, tlb_index        # encoding: [0x00,0x2f,0x43,0x6c]
# DIS-NEXT: mfsr r15, tlb_index
        mfsr    r15, TLB_INDEX
# CHECK: mfsr r6, asid              # encoding: [0x00,0x36,0x43,0x6c]
# DIS-NEXT: mfsr r6, asid
        mfsr    r6, Asid
# CHECK: mfsr r13, tlbvec           # encoding: [0x00,0x4d,0x43,0x6c]
# DIS-NEXT: mfsr r13, tlbvec
        mfsr    r13, tlbvec
# CHECK: mfsr r1, tlb_ptb           # encoding: [0x00,0x51,0x43,0x6c]
# DIS-NEXT: mfsr r1, tlb_ptb
        mfsr    r1, TLB_PTB
# CHECK: mfsr r2, tlb_walk          # encoding: [0x00,0x62,0x43,0x6c]
# DIS-NEXT: mfsr r2, tlb_walk
        mfsr    r2, tlb_walk
# CHECK: mfsr r3, tlb_wcnt          # encoding: [0x00,0x73,0x43,0x6c]
# DIS-NEXT: mfsr r3, tlb_wcnt
        mfsr    r3, Tlb_Wcnt

## MTSR sr, rs1 = 0x6C600000 | sr << 12 | rs1 << 4: every stage 2/3 SR name
# CHECK: mtsr mpu_base0, r3         # encoding: [0x30,0x00,0x61,0x6c]
# DIS-NEXT: mtsr mpu_base0, r3
        mtsr    MPU_BASE0, r3
# CHECK: mtsr mpu_limit0, r8        # encoding: [0x80,0x10,0x61,0x6c]
# DIS-NEXT: mtsr mpu_limit0, r8
        mtsr    Mpu_limit0, r8
# CHECK: mtsr mpu_base1, r13        # encoding: [0xd0,0x20,0x61,0x6c]
# DIS-NEXT: mtsr mpu_base1, r13
        mtsr    mpu_base1, r13
# CHECK: mtsr mpu_limit1, r2        # encoding: [0x20,0x30,0x61,0x6c]
# DIS-NEXT: mtsr mpu_limit1, r2
        mtsr    MPU_LIMIT1, r2
# CHECK: mtsr mpu_base2, r7         # encoding: [0x70,0x40,0x61,0x6c]
# DIS-NEXT: mtsr mpu_base2, r7
        mtsr    Mpu_base2, r7
# CHECK: mtsr mpu_limit2, r12       # encoding: [0xc0,0x50,0x61,0x6c]
# DIS-NEXT: mtsr mpu_limit2, r12
        mtsr    mpu_limit2, r12
# CHECK: mtsr mpu_base3, r1         # encoding: [0x10,0x60,0x61,0x6c]
# DIS-NEXT: mtsr mpu_base3, r1
        mtsr    MPU_BASE3, r1
# CHECK: mtsr mpu_limit3, r6        # encoding: [0x60,0x70,0x61,0x6c]
# DIS-NEXT: mtsr mpu_limit3, r6
        mtsr    Mpu_limit3, r6
# CHECK: mtsr mpu_base4, r11        # encoding: [0xb0,0x80,0x61,0x6c]
# DIS-NEXT: mtsr mpu_base4, r11
        mtsr    mpu_base4, r11
# CHECK: mtsr mpu_limit4, r0        # encoding: [0x00,0x90,0x61,0x6c]
# DIS-NEXT: mtsr mpu_limit4, r0
        mtsr    MPU_LIMIT4, r0
# CHECK: mtsr mpu_base5, r5         # encoding: [0x50,0xa0,0x61,0x6c]
# DIS-NEXT: mtsr mpu_base5, r5
        mtsr    Mpu_base5, r5
# CHECK: mtsr mpu_limit5, r10       # encoding: [0xa0,0xb0,0x61,0x6c]
# DIS-NEXT: mtsr mpu_limit5, r10
        mtsr    mpu_limit5, r10
# CHECK: mtsr mpu_base6, r15        # encoding: [0xf0,0xc0,0x61,0x6c]
# DIS-NEXT: mtsr mpu_base6, r15
        mtsr    MPU_BASE6, r15
# CHECK: mtsr mpu_limit6, r4        # encoding: [0x40,0xd0,0x61,0x6c]
# DIS-NEXT: mtsr mpu_limit6, r4
        mtsr    Mpu_limit6, r4
# CHECK: mtsr mpu_base7, r9         # encoding: [0x90,0xe0,0x61,0x6c]
# DIS-NEXT: mtsr mpu_base7, r9
        mtsr    mpu_base7, r9
# CHECK: mtsr mpu_limit7, r14       # encoding: [0xe0,0xf0,0x61,0x6c]
# DIS-NEXT: mtsr mpu_limit7, r14
        mtsr    MPU_LIMIT7, r14
# CHECK: mtsr mpu_base8, r3         # encoding: [0x30,0x00,0x62,0x6c]
# DIS-NEXT: mtsr mpu_base8, r3
        mtsr    Mpu_base8, r3
# CHECK: mtsr mpu_limit8, r8        # encoding: [0x80,0x10,0x62,0x6c]
# DIS-NEXT: mtsr mpu_limit8, r8
        mtsr    mpu_limit8, r8
# CHECK: mtsr mpu_base9, r13        # encoding: [0xd0,0x20,0x62,0x6c]
# DIS-NEXT: mtsr mpu_base9, r13
        mtsr    MPU_BASE9, r13
# CHECK: mtsr mpu_limit9, r2        # encoding: [0x20,0x30,0x62,0x6c]
# DIS-NEXT: mtsr mpu_limit9, r2
        mtsr    Mpu_limit9, r2
# CHECK: mtsr mpu_base10, r7        # encoding: [0x70,0x40,0x62,0x6c]
# DIS-NEXT: mtsr mpu_base10, r7
        mtsr    mpu_base10, r7
# CHECK: mtsr mpu_limit10, r12      # encoding: [0xc0,0x50,0x62,0x6c]
# DIS-NEXT: mtsr mpu_limit10, r12
        mtsr    MPU_LIMIT10, r12
# CHECK: mtsr mpu_base11, r1        # encoding: [0x10,0x60,0x62,0x6c]
# DIS-NEXT: mtsr mpu_base11, r1
        mtsr    Mpu_base11, r1
# CHECK: mtsr mpu_limit11, r6       # encoding: [0x60,0x70,0x62,0x6c]
# DIS-NEXT: mtsr mpu_limit11, r6
        mtsr    mpu_limit11, r6
# CHECK: mtsr mpu_base12, r11       # encoding: [0xb0,0x80,0x62,0x6c]
# DIS-NEXT: mtsr mpu_base12, r11
        mtsr    MPU_BASE12, r11
# CHECK: mtsr mpu_limit12, r0       # encoding: [0x00,0x90,0x62,0x6c]
# DIS-NEXT: mtsr mpu_limit12, r0
        mtsr    Mpu_limit12, r0
# CHECK: mtsr mpu_base13, r5        # encoding: [0x50,0xa0,0x62,0x6c]
# DIS-NEXT: mtsr mpu_base13, r5
        mtsr    mpu_base13, r5
# CHECK: mtsr mpu_limit13, r10      # encoding: [0xa0,0xb0,0x62,0x6c]
# DIS-NEXT: mtsr mpu_limit13, r10
        mtsr    MPU_LIMIT13, r10
# CHECK: mtsr mpu_base14, r15       # encoding: [0xf0,0xc0,0x62,0x6c]
# DIS-NEXT: mtsr mpu_base14, r15
        mtsr    Mpu_base14, r15
# CHECK: mtsr mpu_limit14, r4       # encoding: [0x40,0xd0,0x62,0x6c]
# DIS-NEXT: mtsr mpu_limit14, r4
        mtsr    mpu_limit14, r4
# CHECK: mtsr mpu_base15, r9        # encoding: [0x90,0xe0,0x62,0x6c]
# DIS-NEXT: mtsr mpu_base15, r9
        mtsr    MPU_BASE15, r9
# CHECK: mtsr mpu_limit15, r14      # encoding: [0xe0,0xf0,0x62,0x6c]
# DIS-NEXT: mtsr mpu_limit15, r14
        mtsr    Mpu_limit15, r14
# CHECK: mtsr tlb_hi, r3            # encoding: [0x30,0x00,0x63,0x6c]
# DIS-NEXT: mtsr tlb_hi, r3
        mtsr    tlb_hi, r3
# CHECK: mtsr tlb_lo, r8            # encoding: [0x80,0x10,0x63,0x6c]
# DIS-NEXT: mtsr tlb_lo, r8
        mtsr    TLB_LO, r8
# CHECK: mtsr tlb_index, r13        # encoding: [0xd0,0x20,0x63,0x6c]
# DIS-NEXT: mtsr tlb_index, r13
        mtsr    Tlb_index, r13
# CHECK: mtsr asid, r2              # encoding: [0x20,0x30,0x63,0x6c]
# DIS-NEXT: mtsr asid, r2
        mtsr    asid, r2
# CHECK: mtsr tlbvec, r7            # encoding: [0x70,0x40,0x63,0x6c]
# DIS-NEXT: mtsr tlbvec, r7
        mtsr    TLBVEC, r7
# CHECK: mtsr tlb_ptb, r4           # encoding: [0x40,0x50,0x63,0x6c]
# DIS-NEXT: mtsr tlb_ptb, r4
        mtsr    tlb_ptb, r4
# CHECK: mtsr tlb_walk, r5          # encoding: [0x50,0x60,0x63,0x6c]
# DIS-NEXT: mtsr tlb_walk, r5
        mtsr    TLB_WALK, r5
# CHECK: mtsr tlb_wcnt, r6          # encoding: [0x60,0x70,0x63,0x6c]
# DIS-NEXT: mtsr tlb_wcnt, r6
        mtsr    tlb_wcnt, r6

## Numbers: a named SR prints as its name, an unnamed one as the number
# CHECK: mfsr r2, mpu_base0         # encoding: [0x00,0x02,0x41,0x6c]
# DIS-NEXT: mfsr r2, mpu_base0
        mfsr    r2, 0x10
# CHECK: mfsr r3, mpu_limit0        # encoding: [0x00,0x13,0x41,0x6c]
# DIS-NEXT: mfsr r3, mpu_limit0
        mfsr    r3, 17
# CHECK: mfsr r4, mpu_base15        # encoding: [0x00,0xe4,0x42,0x6c]
# DIS-NEXT: mfsr r4, mpu_base15
        mfsr    r4, 0x2e
# CHECK: mfsr r5, mpu_limit15       # encoding: [0x00,0xf5,0x42,0x6c]
# DIS-NEXT: mfsr r5, mpu_limit15
        mfsr    r5, 0x2F
# CHECK: mfsr r6, tlb_hi            # encoding: [0x00,0x06,0x43,0x6c]
# DIS-NEXT: mfsr r6, tlb_hi
        mfsr    r6, 48
# CHECK: mfsr r7, tlbvec            # encoding: [0x00,0x47,0x43,0x6c]
# DIS-NEXT: mfsr r7, tlbvec
        mfsr    r7, 0x34
# CHECK: mfsr r8, 9                 # encoding: [0x00,0x98,0x40,0x6c]
# DIS-NEXT: mfsr r8, 9
        mfsr    r8, 9
# CHECK: mfsr r9, 15                # encoding: [0x00,0xf9,0x40,0x6c]
# DIS-NEXT: mfsr r9, 15
        mfsr    r9, 0xf
# CHECK: mfsr r10, tlb_ptb          # encoding: [0x00,0x5a,0x43,0x6c]
# DIS-NEXT: mfsr r10, tlb_ptb
        mfsr    r10, 0x35
# CHECK: mfsr r10, 56               # encoding: [0x00,0x8a,0x43,0x6c]
# DIS-NEXT: mfsr r10, 56
        mfsr    r10, 0x38
# CHECK: mfsr r11, 63               # encoding: [0x00,0xfb,0x43,0x6c]
# DIS-NEXT: mfsr r11, 63
        mfsr    r11, 0x3f
# CHECK: mfsr r12, 64               # encoding: [0x00,0x0c,0x44,0x6c]
# DIS-NEXT: mfsr r12, 64
        mfsr    r12, 0x40
.set SR_TLB_LO, 0x31
# CHECK: mtsr tlb_lo, r1            # encoding: [0x10,0x10,0x63,0x6c]
# DIS-NEXT: mtsr tlb_lo, r1
        mtsr    SR_TLB_LO, r1
# CHECK: mtsr 56, r13               # encoding: [0xd0,0x80,0x63,0x6c]
# DIS-NEXT: mtsr 56, r13
        mtsr    56, r13

## A TLB refill sequence as the handler would write it (MMU_PLAN §11.3).
# CHECK: mfsr r1, tlb_index         # encoding: [0x00,0x21,0x43,0x6c]
# DIS-NEXT: mfsr r1, tlb_index
        mfsr    r1, tlb_index
# CHECK: mtsr tlb_lo, r2            # encoding: [0x20,0x10,0x63,0x6c]
# DIS-NEXT: mtsr tlb_lo, r2
        mtsr    tlb_lo, r2
# CHECK: tlbwi                      # encoding: [0x00,0x00,0x09,0x6c]
# DIS-NEXT: tlbwi
        tlbwi
# CHECK: mtsr asid, r3              # encoding: [0x30,0x30,0x63,0x6c]
# DIS-NEXT: mtsr asid, r3
        mtsr    asid, r3
# CHECK: tlbp                       # encoding: [0x00,0x00,0x0b,0x6c]
# DIS-NEXT: tlbp
        tlbp
# CHECK: tlbinv                     # encoding: [0x00,0x00,0x0c,0x6c]
# DIS-NEXT: tlbinv
        tlbinv

.ifdef ERR
## Names outside the tables, and an instruction name as an SR.
# ERR: [[#@LINE+1]]:21: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec, tlb_ptb, tlb_walk, tlb_wcnt or an integer in the range [0, 255]
        mfsr    r1, mpu_base16
# ERR: [[#@LINE+1]]:21: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec, tlb_ptb, tlb_walk, tlb_wcnt or an integer in the range [0, 255]
        mfsr    r1, mpu_limit16
# ERR: [[#@LINE+1]]:21: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec, tlb_ptb, tlb_walk, tlb_wcnt or an integer in the range [0, 255]
        mfsr    r1, mpu_base
# ERR: [[#@LINE+1]]:21: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec, tlb_ptb, tlb_walk, tlb_wcnt or an integer in the range [0, 255]
        mfsr    r1, mpu_base00
# ERR: [[#@LINE+1]]:21: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec, tlb_ptb, tlb_walk, tlb_wcnt or an integer in the range [0, 255]
        mfsr    r1, mpu_base_0
# ERR: [[#@LINE+1]]:21: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec, tlb_ptb, tlb_walk, tlb_wcnt or an integer in the range [0, 255]
        mfsr    r1, tlb_hi1
# ERR: [[#@LINE+1]]:21: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec, tlb_ptb, tlb_walk, tlb_wcnt or an integer in the range [0, 255]
        mfsr    r1, tlbhi
# ERR: [[#@LINE+1]]:21: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec, tlb_ptb, tlb_walk, tlb_wcnt or an integer in the range [0, 255]
        mfsr    r1, tlbwi
# ERR: [[#@LINE+1]]:21: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec, tlb_ptb, tlb_walk, tlb_wcnt or an integer in the range [0, 255]
        mfsr    r1, mpu_base1+1
# ERR: [[#@LINE+1]]:17: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec, tlb_ptb, tlb_walk, tlb_wcnt or an integer in the range [0, 255]
        mtsr    mpu_limit16, r1
# ERR: [[#@LINE+1]]:17: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec, tlb_ptb, tlb_walk, tlb_wcnt or an integer in the range [0, 255]
        mtsr    tlb_idx, r1
# ERR: [[#@LINE+1]]:17: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec, tlb_ptb, tlb_walk, tlb_wcnt or an integer in the range [0, 255]
        mtsr    asid0, r1
# ERR: [[#@LINE+1]]:17: error: system register must be status, evec, ecause, eaddr, usp, scratch0, scratch1, tp, cycle, mpu_base0..mpu_base15, mpu_limit0..mpu_limit15, tlb_hi, tlb_lo, tlb_index, asid, tlbvec, tlb_ptb, tlb_walk, tlb_wcnt or an integer in the range [0, 255]
        mtsr    r1, asid
## An SR name where a GPR goes.
# ERR: [[#@LINE+1]]:17: error: invalid operand for instruction
        mfsr    asid, r1
## The TLB instructions take no operands.
# ERR: [[#@LINE+1]]:17: error: invalid operand for instruction
        tlbwi   r1
# ERR: [[#@LINE+1]]:17: error: invalid operand for instruction
        tlbr    0
# ERR: [[#@LINE+1]]:17: error: invalid operand for instruction
        tlbp    r2
# ERR: [[#@LINE+1]]:17: error: invalid operand for instruction
        tlbinv  r1, r2
.endif
