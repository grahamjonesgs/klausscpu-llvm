# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -show-encoding %s \
# RUN:   | FileCheck %s
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj %s -o %t.o
# RUN: llvm-readobj -r %t.o | FileCheck %s --check-prefix=RELOC
# RUN: llvm-readelf -s %t.o | FileCheck %s --check-prefix=SYM
# RUN: llvm-objdump -d --no-show-raw-insn %t.o | FileCheck %s --check-prefix=DIS
## The printed form reassembles to the same object.
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl %s \
# RUN:   | llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj -o %t2.o
# RUN: cmp %t.o %t2.o

# TLS local exec: `%tprel(sym)` is the offset of TLS symbol sym from the
# thread pointer (TP, `mfsr rd, tp`), FK_KlaussCPU_TPREL32 in the word-1 slot
# of ADDI, the indexed loads and stores and SETR, R_KLAUSSCPU_TPREL32 in the
# object.  A symbol it names becomes STT_TLS, defined or not.  An immediate
# that fits stays a short form; %tprel always keeps the 2-word form.

# CHECK:      mfsr r1, tp # encoding: [0x00,0x71,0x40,0x6c]
        mfsr    r1, tp
# CHECK:      addi r2, r1, %tprel(a) # encoding: [0x10,0x02,0x30,0x88,A,A,A,A]
# CHECK-NEXT: fixup A - offset: 4, value: %tprel(a), kind: FK_KlaussCPU_TPREL32
        addi    r2, r1, %tprel(a)
# CHECK:      addi r2, r1, %tprel(a+8) # encoding: [0x10,0x02,0x30,0x88,A,A,A,A]
# CHECK-NEXT: fixup A - offset: 4, value: %tprel(a+8), kind: FK_KlaussCPU_TPREL32
        addi    r2, r1, %tprel(a+8)
## An addend after the parenthesis joins the expression; the specifier is
## case-insensitive.
# CHECK:      addi r2, r1, %tprel(a+12) # encoding: [0x10,0x02,0x30,0x88,A,A,A,A]
        addi    r2, r1, %TPREL(a)+12
# CHECK:      ldidx64 r3, r1, %tprel(b) # encoding: [0x10,0x03,0x30,0x9b,A,A,A,A]
# CHECK-NEXT: fixup A - offset: 4, value: %tprel(b), kind: FK_KlaussCPU_TPREL32
        ldidx64 r3, r1, %tprel(b)
# CHECK:      ldidx64u r3, r1, %tprel(b) # encoding: [0x10,0x03,0x20,0x9b,A,A,A,A]
        ldidx64u r3, r1, %tprel(b)
# CHECK:      ldidx32 r3, r1, %tprel(b+4) # encoding: [0x10,0x03,0x20,0x9a,A,A,A,A]
        ldidx32 r3, r1, %tprel(b+4)
# CHECK:      ldidx32s r3, r1, %tprel(b) # encoding: [0x10,0x03,0xa0,0x9a,A,A,A,A]
        ldidx32s r3, r1, %tprel(b)
# CHECK:      ldidx16 r3, r1, %tprel(b) # encoding: [0x10,0x03,0x20,0x99,A,A,A,A]
        ldidx16 r3, r1, %tprel(b)
# CHECK:      ldidx16s r3, r1, %tprel(b) # encoding: [0x10,0x03,0xa0,0x99,A,A,A,A]
        ldidx16s r3, r1, %tprel(b)
# CHECK:      ldidx8 r3, r1, %tprel(b) # encoding: [0x10,0x03,0x20,0x98,A,A,A,A]
        ldidx8  r3, r1, %tprel(b)
# CHECK:      ldidx8s r3, r1, %tprel(b) # encoding: [0x10,0x03,0xa0,0x98,A,A,A,A]
        ldidx8s r3, r1, %tprel(b)
# CHECK:      stidx64 r3, r1, %tprel(c) # encoding: [0x10,0x03,0x30,0x9f,A,A,A,A]
# CHECK-NEXT: fixup A - offset: 4, value: %tprel(c), kind: FK_KlaussCPU_TPREL32
        stidx64 r3, r1, %tprel(c)
# CHECK:      stidx64u r3, r1, %tprel(c) # encoding: [0x10,0x03,0x20,0x9f,A,A,A,A]
        stidx64u r3, r1, %tprel(c)
# CHECK:      stidx32 r3, r1, %tprel(c) # encoding: [0x10,0x03,0x20,0x9e,A,A,A,A]
        stidx32 r3, r1, %tprel(c)
# CHECK:      stidx16 r3, r1, %tprel(c) # encoding: [0x10,0x03,0x20,0x9d,A,A,A,A]
        stidx16 r3, r1, %tprel(c)
# CHECK:      stidx8 r3, r1, %tprel(c) # encoding: [0x10,0x03,0x20,0x9c,A,A,A,A]
        stidx8  r3, r1, %tprel(c)
## SETR sign-extends the (negative) offset.
# CHECK:      setr r4, %tprel(c-16) # encoding: [0x00,0x04,0xd0,0x8b,A,A,A,A]
# CHECK-NEXT: fixup A - offset: 4, value: %tprel(c-16), kind: FK_KlaussCPU_TPREL32
        setr    r4, %tprel(c-16)
## A defined TLS symbol keeps its own name in the relocation.
# CHECK:      ldidx32 r3, r1, %tprel(local+4) # encoding: [0x10,0x03,0x20,0x9a,A,A,A,A]
        ldidx32 r3, r1, %tprel(local+4)
## Constants are unchanged, and still compress.
# CHECK:      addi r2, r1, 8 # encoding: [0x10,0x82,0x30,0x48]
        addi    r2, r1, 8
# CHECK:      ldidx64 r3, r1, -8 # encoding: [0x10,0xf3,0x3f,0x5b]
        ldidx64 r3, r1, -8
# CHECK:      addi r2, r1, -2147483648 # encoding: [0x10,0x02,0x30,0x88,0x00,0x00,0x00,0x80]
        addi    r2, r1, -2147483648

        .section .tbss,"awT",@nobits
        .p2align 3
local:
        .zero 16

# RELOC:      Section ({{[0-9]+}}) .rela.text {
# RELOC-NEXT:   0x8 R_KLAUSSCPU_TPREL32 a 0x0
# RELOC-NEXT:   0x10 R_KLAUSSCPU_TPREL32 a 0x8
# RELOC-NEXT:   0x18 R_KLAUSSCPU_TPREL32 a 0xC
# RELOC-NEXT:   0x20 R_KLAUSSCPU_TPREL32 b 0x0
# RELOC-NEXT:   0x28 R_KLAUSSCPU_TPREL32 b 0x0
# RELOC-NEXT:   0x30 R_KLAUSSCPU_TPREL32 b 0x4
# RELOC-NEXT:   0x38 R_KLAUSSCPU_TPREL32 b 0x0
# RELOC-NEXT:   0x40 R_KLAUSSCPU_TPREL32 b 0x0
# RELOC-NEXT:   0x48 R_KLAUSSCPU_TPREL32 b 0x0
# RELOC-NEXT:   0x50 R_KLAUSSCPU_TPREL32 b 0x0
# RELOC-NEXT:   0x58 R_KLAUSSCPU_TPREL32 b 0x0
# RELOC-NEXT:   0x60 R_KLAUSSCPU_TPREL32 c 0x0
# RELOC-NEXT:   0x68 R_KLAUSSCPU_TPREL32 c 0x0
# RELOC-NEXT:   0x70 R_KLAUSSCPU_TPREL32 c 0x0
# RELOC-NEXT:   0x78 R_KLAUSSCPU_TPREL32 c 0x0
# RELOC-NEXT:   0x80 R_KLAUSSCPU_TPREL32 c 0x0
# RELOC-NEXT:   0x88 R_KLAUSSCPU_TPREL32 c 0xFFFFFFF0
# RELOC-NEXT:   0x90 R_KLAUSSCPU_TPREL32 local 0x4
# RELOC-NEXT: }

# SYM-DAG: TLS     LOCAL  DEFAULT {{[0-9]+}} local
# SYM-DAG: TLS     GLOBAL DEFAULT   UND a
# SYM-DAG: TLS     GLOBAL DEFAULT   UND b
# SYM-DAG: TLS     GLOBAL DEFAULT   UND c

# DIS:      0: mfsr r1, tp
# DIS-NEXT: 4: addi r2, r1, 0
# DIS:     1c: ldidx64 r3, r1, 0
# DIS:     84: setr r4, 0
