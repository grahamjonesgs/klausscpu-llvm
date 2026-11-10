# REQUIRES: klausscpu
## TLS local exec, R_KLAUSSCPU_TPREL32: the offset of a TLS symbol from the
## thread pointer, TLS variant II as musl lays it out by default (no
## TLS_ABOVE_TP; src/env/__init_tls.c static_init_tls and __copy_tls): TP
## is aligned to p_align and the TLS block ends just below it, padded so that
## its start is congruent to p_vaddr modulo p_align:
##   size   = p_memsz + ((-p_memsz - p_vaddr) & (p_align - 1))
##   tpoff  = (S - p_vaddr) - size + A
## i.e. lld's variant II, S - p_memsz - ((-p_vaddr - p_memsz) & (p_align - 1)).

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj main.s -o main.o
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj other.s -o other.o

## .tdata at 0x1004, so p_vaddr is not a multiple of p_align (64, from c):
##   .tdata 0x1004: a (4 bytes); .tbss 0x1040 (64-aligned): b (8), c at 0x1080
##   p_memsz = 0x1084 - 0x1004 = 0x80
##   size = 0x80 + ((-0x80 - 0x1004) & 63) = 0x80 + 0x3c = 0xbc
##   a: 0x00 - 0xbc = -188   b: 0x3c - 0xbc = -128   c: 0x7c - 0xbc = -64
## (b and c keep their alignment: TP is 64-aligned.)
# RUN: echo 'SECTIONS { .text 0x100 : { *(.text) } \
# RUN:   .tdata 0x1004 : { *(.tdata) } .tbss : { *(.tbss) } }' > a.lds
# RUN: ld.lld -static -T a.lds main.o -o a
# RUN: llvm-readelf -l a | FileCheck %s --check-prefix=PHDR
# RUN: llvm-objdump -d --no-show-raw-insn a | FileCheck %s

# PHDR: TLS 0x{{[0-9a-f]+}} 0x00001004 0x00001004 0x00004 0x00080 R 0x40

# CHECK:      <_start>:
# CHECK-NEXT:   mfsr r1, tp
# CHECK-NEXT:   addi r2, r1, -188
# CHECK-NEXT:   ldidx32 r3, r1, -188
# CHECK-NEXT:   ldidx64 r3, r1, -128
# CHECK-NEXT:   addi r2, r1, -124
# CHECK-NEXT:   stidx32 r3, r1, -64
# CHECK-NEXT:   setr r4, -64
## An undefined weak TLS symbol: A.
# CHECK-NEXT:   addi r2, r1, 16

## Default layout: .tdata and .tbss in the RW segment, p_vaddr 64-aligned.
##   .tdata: a (4) at 0, d (8) at 8; .tbss (64-aligned): b at 0x40, c at 0x80
##   p_memsz = 0x84, p_align = 64
##   size = 0x84 + ((-0x84 - p_vaddr) & 63) = 0x84 + 0x3c = 0xc0
##   a: -192   d: 8 - 192 = -184   b: 64 - 192 = -128   c: -64
# RUN: ld.lld -static main.o other.o -o b
# RUN: llvm-readelf -l b | FileCheck %s --check-prefix=PHDRB
# RUN: llvm-objdump -d --no-show-raw-insn b | FileCheck %s --check-prefix=B
# PHDRB: TLS 0x{{[0-9a-f]+}} 0x{{[0-9a-f]+}}{{[048c]}}0 0x{{[0-9a-f]+}} 0x00010 0x00084 R 0x40
# B:      <_start>:
# B-NEXT:   mfsr r1, tp
# B-NEXT:   addi r2, r1, -192
# B-NEXT:   ldidx32 r3, r1, -192
# B-NEXT:   ldidx64 r3, r1, -128
# B-NEXT:   addi r2, r1, -124
# B-NEXT:   stidx32 r3, r1, -64
# B-NEXT:   setr r4, -64
# B-NEXT:   addi r2, r1, 16
# B:      <other>:
# B-NEXT:   ldidx64 r3, r1, -184

## Errors: no TP-relative relocation in a shared object, nor against a
## symbol that is not TLS.
# RUN: not ld.lld -shared main.o other.o -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=SHARED
# SHARED: error: main.o:(.text+0x8): relocation R_KLAUSSCPU_TPREL32 against a cannot be used with -shared
## A TPREL relocation against a symbol another object defines outside TLS.
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj notls.s -o notls.o
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj data.s -o data.o
# RUN: not ld.lld -static notls.o data.o -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=NOTLS
# NOTLS: error: notls.o:(.text+0x4): relocation R_KLAUSSCPU_TPREL32 against non-TLS symbol data

#--- main.s
        .text
        .globl _start
_start:
        mfsr    r1, tp
        addi    r2, r1, %tprel(a)
        ldidx32 r3, r1, %tprel(a)
        ldidx64 r3, r1, %tprel(b)
        addi    r2, r1, %tprel(b+4)
        stidx32 r3, r1, %tprel(c)
        setr    r4, %tprel(c)
        .weak   undefweak
        addi    r2, r1, %tprel(undefweak+16)

        .section .tdata,"awT",@progbits
        .globl a
        .p2align 2
a:      .long 42

        .section .tbss,"awT",@nobits
        .globl b, c
        .p2align 3
b:      .zero 8
        .p2align 6
c:      .zero 4

#--- other.s
        .text
        .globl other
other:
        ldidx64 r3, r1, %tprel(d)

        .section .tdata,"awT",@progbits
        .p2align 3
d:      .quad 7

#--- notls.s
        .text
        .globl _start
_start:
        addi    r2, r1, %tprel(data)

#--- data.s
        .data
        .globl data
data:   .long 0
