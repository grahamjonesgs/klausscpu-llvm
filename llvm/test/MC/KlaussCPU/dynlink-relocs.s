# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -show-encoding %s \
# RUN:   | FileCheck %s
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj %s -o %t.o
# RUN: llvm-readobj -r %t.o | FileCheck %s --check-prefix=RELOC
# RUN: llvm-readelf -s %t.o | FileCheck %s --check-prefix=SYM
# RUN: llvm-objdump -dr --no-show-raw-insn %t.o | FileCheck %s --check-prefix=DIS
## The printed form reassembles to the same object.
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl %s \
# RUN:   | llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj -o %t2.o
# RUN: cmp %t.o %t2.o
## The 2-word forms do not depend on the short forms.
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj \
# RUN:   -mattr=-short-forms,-short-branches,-short-calls %s -o %t3.o
# RUN: llvm-readobj -r %t3.o | FileCheck %s --check-prefix=RELOC-LONG

# Dynamic linking (ABI.md): `%gotpcrel(sym)`, `%gottprel(sym)` and
# `%tlsgd(sym)` in the word-1 immediate of the 2-word leapc, `%plt(sym)` in
# that of the 2-word callrel / jmprel / tail.  Each is relative to the
# instruction like PCREL32, never resolved by the assembler (not even for a
# local symbol in the same section), against the symbol itself (not its
# section) with addend 0, and never compressed: R_KLAUSSCPU_GOTPCREL32 (8),
# PLT32 (9), TLS_GOTTPREL32 (10), TLS_GD32 (11).  %gottprel and %tlsgd make
# their symbol STT_TLS.

        .text
        .globl  f
f:
# CHECK:      leapc r1, %gotpcrel(ext) # encoding: [0x00,0x01,0x80,0x8b,A,A,A,A]
# CHECK-NEXT: fixup A - offset: 4, value: %gotpcrel(ext), kind: FK_KlaussCPU_GOTPCREL32
        leapc   r1, %gotpcrel(ext)
## Case-insensitive; a symbol defined here, global or local, in this
## section too.
# CHECK:      leapc r2, %gotpcrel(f) # encoding: [0x00,0x02,0x80,0x8b,A,A,A,A]
# CHECK-NEXT: fixup A - offset: 4, value: %gotpcrel(f), kind: FK_KlaussCPU_GOTPCREL32
        leapc   r2, %GOTPCREL(f)
# CHECK:      leapc r3, %gotpcrel(local) # encoding: [0x00,0x03,0x80,0x8b,A,A,A,A]
        leapc   r3, %gotpcrel(local)
# CHECK:      leapc r4, %gotpcrel(data) # encoding: [0x00,0x04,0x80,0x8b,A,A,A,A]
        leapc   r4, %gotpcrel(data)
# CHECK:      leapc r5, %gottprel(tv) # encoding: [0x00,0x05,0x80,0x8b,A,A,A,A]
# CHECK-NEXT: fixup A - offset: 4, value: %gottprel(tv), kind: FK_KlaussCPU_TLS_GOTTPREL32
        leapc   r5, %gottprel(tv)
# CHECK:      leapc r0, %tlsgd(tv) # encoding: [0x00,0x00,0x80,0x8b,A,A,A,A]
# CHECK-NEXT: fixup A - offset: 4, value: %tlsgd(tv), kind: FK_KlaussCPU_TLS_GD32
        leapc   r0, %tlsgd(tv)
# CHECK:      leapc r0, %tlsgd(tl) # encoding: [0x00,0x00,0x80,0x8b,A,A,A,A]
        leapc   r0, %tlsgd(tl)
# CHECK:      callrel %plt(g) # encoding: [0x00,0x00,0x00,0xa3,A,A,A,A]
# CHECK-NEXT: fixup A - offset: 4, value: %plt(g), kind: FK_KlaussCPU_PLT32
        callrel %plt(g)
# CHECK:      callrel %plt(local) # encoding: [0x00,0x00,0x00,0xa3,A,A,A,A]
        callrel %plt(local)
# CHECK:      jmprel %plt(g) # encoding: [0x00,0x00,0x00,0xa1,A,A,A,A]
# CHECK-NEXT: fixup A - offset: 4, value: %plt(g), kind: FK_KlaussCPU_PLT32
        jmprel  %plt(g)
# CHECK:      tail %plt(f) # encoding: [0x00,0x00,0x00,0xa1,A,A,A,A]
# CHECK-NEXT: fixup A - offset: 4, value: %plt(f), kind: FK_KlaussCPU_PLT32
        tail    %plt(f)
## Without a specifier: the short forms, as before.
# CHECK:      callrel g # encoding: [A,A,0b000000AA,0x63]
        callrel g
# CHECK:      leapc r6, local # encoding: [A,0x06'A',0b1010AAAA,0x4b]
        leapc   r6, local
local:
        ret

        .data
data:   .quad 0
        .section .tbss,"awT",@nobits
tl:     .zero 8

# RELOC:      Section (3) .rela.text {
# RELOC-NEXT:   0x4 R_KLAUSSCPU_GOTPCREL32 ext 0x0
# RELOC-NEXT:   0xC R_KLAUSSCPU_GOTPCREL32 f 0x0
# RELOC-NEXT:   0x14 R_KLAUSSCPU_GOTPCREL32 local 0x0
# RELOC-NEXT:   0x1C R_KLAUSSCPU_GOTPCREL32 data 0x0
# RELOC-NEXT:   0x24 R_KLAUSSCPU_TLS_GOTTPREL32 tv 0x0
# RELOC-NEXT:   0x2C R_KLAUSSCPU_TLS_GD32 tv 0x0
# RELOC-NEXT:   0x34 R_KLAUSSCPU_TLS_GD32 tl 0x0
# RELOC-NEXT:   0x3C R_KLAUSSCPU_PLT32 g 0x0
# RELOC-NEXT:   0x44 R_KLAUSSCPU_PLT32 local 0x0
# RELOC-NEXT:   0x4C R_KLAUSSCPU_PLT32 g 0x0
# RELOC-NEXT:   0x54 R_KLAUSSCPU_PLT32 f 0x0
# RELOC-NEXT:   0x58 R_KLAUSSCPU_CALL18 g 0x0
# RELOC-NEXT: }

# RELOC-LONG:      0x54 R_KLAUSSCPU_PLT32 f 0x0
# RELOC-LONG-NEXT: 0x5C R_KLAUSSCPU_PCREL32 g 0x0
# RELOC-LONG-NEXT: }

# SYM-DAG: NOTYPE  LOCAL  DEFAULT {{[0-9]+}} local
# SYM-DAG: NOTYPE  LOCAL  DEFAULT {{[0-9]+}} data
# SYM-DAG: TLS     LOCAL  DEFAULT {{[0-9]+}} tl
# SYM-DAG: TLS     GLOBAL DEFAULT   UND tv
# SYM-DAG: NOTYPE  GLOBAL DEFAULT   UND ext

# DIS:      0: leapc r1, 0x0
# DIS-NEXT:    00000004: R_KLAUSSCPU_GOTPCREL32 ext
# DIS:     20: leapc r5, 0x20
# DIS-NEXT:    00000024: R_KLAUSSCPU_TLS_GOTTPREL32 tv
# DIS:     28: leapc r0, 0x28
# DIS-NEXT:    0000002c: R_KLAUSSCPU_TLS_GD32 tv
# DIS:     38: callrel 0x38 <f+0x38>
# DIS-NEXT:    0000003c: R_KLAUSSCPU_PLT32 g
# DIS:     48: jmprel 0x48 <f+0x48>
# DIS-NEXT:    0000004c: R_KLAUSSCPU_PLT32 g
# DIS:     50: jmprel 0x50 <f+0x50>
# DIS-NEXT:    00000054: R_KLAUSSCPU_PLT32 f
# DIS:     58: callrel 0x58 <f+0x58>
# DIS-NEXT:    00000058: R_KLAUSSCPU_CALL18 g
# DIS:     5c: leapc r6, 0x60{{$}}
