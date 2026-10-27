# RUN: llvm-mc -triple=klausscpu -filetype=obj -klausscpu-short-calls=false \
# RUN:   %s -o %t.o
# RUN: llvm-objdump -dr --no-show-raw-insn %t.o | FileCheck %s
# RUN: llvm-objdump -d %t.o | FileCheck %s --check-prefix=BYTES

# FK_KlaussCPU_PCREL32 (LEAPC, JMPxxREL, CALLREL word 1) is a PC-relative
# fixup.  A target MC cannot resolve in-section -- here an absolute .set
# symbol -- gets an R_KLAUSSCPU_PCREL32 relocation with a zero field; it used
# to get no relocation, so the instruction branched to (or LEAPC returned)
# its own address.  A .set before the use must not turn the operand into a
# displacement either.  A resolved in-section target needs no relocation and
# its field is target - instruction address.  (Calls stay long here; the
# short call's R_KLAUSSCPU_CALL18 is tested in short-call.s.)

        .set BEFORE, 0x1000

# CHECK-LABEL: <func>:
func:
# CHECK-NEXT:   0: leapc r1, 0x0
# CHECK-NEXT:        00000004: R_KLAUSSCPU_PCREL32 *ABS*+0x1000
        leapc   r1, BEFORE
# CHECK-NEXT:   8: callrel 0x8
# CHECK-NEXT:        0000000c: R_KLAUSSCPU_PCREL32 *ABS*+0x1000
        callrel BEFORE
# CHECK-NEXT:  10: jmprel 0x10
# CHECK-NEXT:        00000014: R_KLAUSSCPU_PCREL32 *ABS*+0x1000
        jmprel  BEFORE
# CHECK-NEXT:  18: jmpnerel 0x18
# CHECK-NEXT:        0000001c: R_KLAUSSCPU_PCREL32 *ABS*+0x1008
        jmpnerel BEFORE+8
## The absolute forms take the value directly.
# CHECK-NEXT:  20: call 0x1000
        call    BEFORE
# CHECK-NEXT:  28: leapc r1, 0x28
# CHECK-NEXT:        0000002c: R_KLAUSSCPU_PCREL32 *ABS*+0x2000
        leapc   r1, AFTER
# CHECK-NEXT:  30: callrel 0x30
# CHECK-NEXT:        00000034: R_KLAUSSCPU_PCREL32 *ABS*+0x2000
        callrel AFTER
## A short jmp to a target that is not in-section relaxes to jmprel.
# CHECK-NEXT:  38: jmprel 0x38
# CHECK-NEXT:        0000003c: R_KLAUSSCPU_PCREL32 *ABS*+0x2000
        jmp     AFTER
# CHECK-NEXT:  40: call 0x2000
        call    AFTER
# CHECK-NEXT:  48: callrel 0x48
# CHECK-NEXT:        0000004c: R_KLAUSSCPU_PCREL32 ext
        callrel ext
## Resolved in-section: no relocation, field = target - instruction address.
# CHECK-NEXT:  50: leapc r2, 0x60
# CHECK-NEXT:  58: callrel 0x60 <loc>
# CHECK-EMPTY:
# CHECK-NEXT:  <loc>:
# CHECK-NEXT:  60: leapc r3, 0x0
# CHECK-NEXT:  68: ret
        leapc   r2, loc
        callrel loc
loc:
        leapc   r3, func
        ret
## Any spelling that names the symbol is a target, not only a leading name.
# CHECK-NEXT:  6c: callrel 0x6c
# CHECK-NEXT:        00000070: R_KLAUSSCPU_PCREL32 *ABS*+0x1000
        callrel (BEFORE)
# CHECK-NEXT:  74: callrel 0x74
# CHECK-NEXT:        00000078: R_KLAUSSCPU_PCREL32 *ABS*+0x1004
        callrel 4+BEFORE
# CHECK-NEXT:  7c: leapc r4, 0x7c
# CHECK-NEXT:        00000080: R_KLAUSSCPU_PCREL32 *ABS*-0x1000
        leapc   r4, -BEFORE
## A redefined symbol keeps its value at each use.
        .set REDEF, 0x3000
# CHECK-NEXT:  84: callrel 0x84
# CHECK-NEXT:        00000088: R_KLAUSSCPU_PCREL32 *ABS*+0x3000
        callrel REDEF
        .set REDEF, 0x4000
# CHECK-NEXT:  8c: callrel 0x8c
# CHECK-NEXT:        00000090: R_KLAUSSCPU_PCREL32 *ABS*+0x4000
        callrel REDEF
## A bare number is the displacement.
# CHECK-NEXT:  94: callrel 0xb4
# CHECK-NEXT:  9c: leapc r5, 0x9c
        callrel 0x20
        leapc   r5, 0
# CHECK-NOT: {{.}}

        .set AFTER, 0x2000

# BYTES:      0: 00 01 80 8b 00 00 00 00
# BYTES:      8: 00 00 00 a3 00 00 00 00
# BYTES:     50: 00 02 80 8b 10 00 00 00
# BYTES-NEXT: 58: 00 00 00 a3 08 00 00 00
# BYTES:      60: 00 03 80 8b a0 ff ff ff
