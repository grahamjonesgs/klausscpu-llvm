# RUN: llvm-mc -triple=klausscpu-unknown-elf -filetype=obj %s -o %t.o
# RUN: llvm-objdump -dr %t.o | FileCheck %s
# RUN: llvm-mc -triple=klausscpu-unknown-elf -filetype=obj -mc-relax-all %s \
# RUN:   -o %t.all.o
# RUN: llvm-objdump -dr %t.all.o | FileCheck %s --check-prefix=ALL
# RUN: llvm-mc -triple=klausscpu-unknown-elf -filetype=obj \
# RUN:   -klausscpu-short-forms=false %s -o %t.long.o
# RUN: llvm-objdump -dr %t.long.o | FileCheck %s --check-prefix=LONG

# LEAPC.S (hw-proposals 9, klaussCPU ISA_ENCODING_V2_MAP §8.3): rd = PC +
# sext(simm16), the byte offset split over word0[19:12] and [7:0]
# (FK_KlaussCPU_PCREL16).  A LEAPC to a symbol starts short; the asm backend
# keeps it when the target is resolved in its section and within simm16 of
# the instruction, and relaxes it to the 2-word LEAPC otherwise: out of
# range, in another section (R_KLAUSSCPU_PCREL32 as before), undefined or
# absolute.  Relaxing one moves the targets of the others, so the range is
# checked on the final layout.

# CHECK:      0: 00 01 a0 4b leapc r1, 0x0
# CHECK-NEXT: 4: 00 02 80 8b 04 80 00 00 leapc r2, 0x8008
# CHECK-NEXT: c: fc f3 a7 4b leapc r3, 0x8008
# CHECK-NEXT: 10: 00 04 80 8b 00 00 00 00 leapc r4, 0x10
# CHECK-NEXT: R_KLAUSSCPU_PCREL32 .data+0x8
# CHECK-NEXT: 18: 00 05 80 8b 00 00 00 00 leapc r5, 0x18
# CHECK-NEXT: R_KLAUSSCPU_PCREL32 ext
# CHECK-NEXT: 20: 00 06 80 8b 00 00 00 00 leapc r6, 0x20
# CHECK-NEXT: R_KLAUSSCPU_PCREL32 *ABS*+0x100
# CHECK-EMPTY:
# CHECK-NEXT: <mid>:
# CHECK-NEXT: 28: e0 f7 af 4b leapc r7, 0x8
# CHECK-NEXT: 2c: 00 08 a8 4b leapc r8, 0xffff802c

# -mc-relax-all relaxes every LEAPC.S with a symbol, not a literal one.
# ALL:      0: 00 01 80 8b 00 00 00 00 leapc r1, 0x0
# ALL:      30: 00 07 80 8b d8 ff ff ff leapc r7, 0x8
# ALL-NEXT: 38: 00 08 a8 4b leapc r8, 0xffff8038

# Without the short forms every LEAPC is the 2-word one.
# LONG:      0: 00 01 80 8b 00 00 00 00 leapc r1, 0x0
# LONG:      38: 00 08 80 8b 00 80 ff ff leapc r8, 0xffff8038

        .set ABSSYM, 0x100
start:
        leapc   r1, start
        leapc   r2, last
        leapc   r3, last
        leapc   r4, data
        leapc   r5, ext
        leapc   r6, ABSSYM
mid:
        leapc   r7, start+8
        leapc   r8, -32768
        .space  32728
last:
        nop

        .data
        .quad   0
data:
        .quad   1
