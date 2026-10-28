# RUN: llvm-mc -triple=klausscpu -filetype=obj %s -o %t.o
# RUN: llvm-objdump -dr %t.o | FileCheck %s
# RUN: llvm-mc -triple=klausscpu -filetype=obj -klausscpu-short-calls=false \
# RUN:   %s -o %t.long.o
# RUN: llvm-objdump -dr --no-show-raw-insn %t.long.o \
# RUN:   | FileCheck %s --check-prefix=LONG
# RUN: llvm-mc -triple=klausscpu %s | FileCheck %s --check-prefix=ASM
# RUN: llvm-mc -triple=klausscpu %s | llvm-mc -triple=klausscpu -filetype=obj \
# RUN:   -o %t.re.o
# RUN: cmp %t.o %t.re.o

# `tail` is the jump of a tail call: JMPREL's encoding (the disassembler shows
# jmprel), and with a symbol the 1-word short jump (0x61000000 | simm18)
# whose target is fixed up like a short call's: resolved here for a local
# callee in the same section (relaxed to the 2-word form out of reach), else
# an R_KLAUSSCPU_CALL18 relocation (lld adds a jmprel thunk when out of
# reach).  A literal displacement stays 2-word, and without short calls every
# `tail` is the 2-word JMPREL with an R_KLAUSSCPU_PCREL32.

# CHECK-LABEL: <f>:
# CHECK-NEXT:   0: 03 00 00 61   jmprel 0xc <near>
# CHECK-NEXT:   4: 00 00 00 61   jmprel 0x4 <f+0x4>
# CHECK-NEXT:        00000004:  R_KLAUSSCPU_CALL18 ext
# CHECK-NEXT:   8: 00 00 00 61   jmprel 0x8 <f+0x8>
# CHECK-NEXT:        00000008:  R_KLAUSSCPU_CALL18 ext+0x10
# LONG-LABEL: <f>:
# LONG-NEXT:    0: jmprel 0x18 <near>
# LONG-NEXT:    8: jmprel 0x8 <f+0x8>
# LONG-NEXT:        0000000c:  R_KLAUSSCPU_PCREL32 ext
# LONG-NEXT:   10: jmprel 0x10 <f+0x10>
# LONG-NEXT:        00000014:  R_KLAUSSCPU_PCREL32 ext+0x10
# ASM:      tail near
# ASM:      tail ext
# ASM:      tail ext+16
# ASM:      tail 16
# ASM:      tail far
f:
        tail    near
        tail    ext
        tail    ext+16
# CHECK-LABEL: <near>:
# CHECK-NEXT:   c: 00 00 00 a1 10 00 00 00   jmprel 0x1c <near+0x10>
## A callee 512 KiB ahead: relaxed.
# CHECK-NEXT:  14: 00 00 00 a1 08 00 08 00   jmprel 0x8001c <far>
near:
        tail    16
        tail    far
        .fill   0x80000, 1, 0
far:
        ret
