# REQUIRES: klausscpu
## --apply-dynamic-relocs writes each dynamic relocation's addend into its
## 64-bit field, sign-extended (ABI.md "Dynamic linking": A is an Elf32_Sword
## sign-extended for every type), and --check-dynamic-relocations reads it
## back (getImplicitAddend).  Without the option the fields hold 0, except
## what lld fills itself (the DTP offset of a local TLS variable).

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj a.s -o a.o
# RUN: ld.lld -shared --apply-dynamic-relocs --check-dynamic-relocations a.o -o a.so
# RUN: llvm-readelf -r a.so | FileCheck %s --check-prefix=REL
# RUN: llvm-objdump -s -j .got -j .data a.so | FileCheck %s --check-prefix=APPLY
# RUN: ld.lld -shared a.o -o noapply.so
# RUN: llvm-objdump -s -j .got -j .data noapply.so | FileCheck %s --check-prefix=NOAPPLY

# REL:      00020358 0000000f R_KLAUSSCPU_RELATIVE 30378
# REL-NEXT: 00030390 0000000f R_KLAUSSCPU_RELATIVE fffffffffff30378
# REL-NEXT: 00020360 00000011 R_KLAUSSCPU_DTPMOD64 0
# REL-NEXT: 00020370 00000010 R_KLAUSSCPU_TPOFF64 8
# REL-NEXT: 00030398 00000102 R_KLAUSSCPU_ABS64 00000000 ext - 10
# REL-NEXT: 00020338 0000030d R_KLAUSSCPU_GLOB_DAT 00030380 var + 0
# REL-NEXT: 00030388 00000302 R_KLAUSSCPU_ABS64 00030380 var - 8
# REL-NEXT: 00020340 00000411 R_KLAUSSCPU_DTPMOD64 00000010 tg + 0
# REL-NEXT: 00020348 00000412 R_KLAUSSCPU_DTPOFF64 00000010 tg + 0
# REL-NEXT: 00020350 00000410 R_KLAUSSCPU_TPOFF64 00000010 tg + 0

## .got: var (GLOB_DAT), tg's GD pair, tg (TPOFF64), loc (RELATIVE), tl's GD
## pair (DTPMOD64, then its offset 8), tl (TPOFF64 + 8).
# APPLY:      Contents of section .got:
# APPLY-NEXT:  20338 00000000 00000000 00000000 00000000
# APPLY-NEXT:  20348 00000000 00000000 00000000 00000000
# APPLY-NEXT:  20358 78030300 00000000 00000000 00000000
# APPLY-NEXT:  20368 08000000 00000000 08000000 00000000
## .data: var - 8, loc - 0x100000, ext - 16: negative, sign-extended.
# APPLY:      Contents of section .data:
# APPLY-NEXT:  30378 00000000 00000000 00000000 00000000
# APPLY-NEXT:  30388 f8ffffff ffffffff 7803f3ff ffffffff
# APPLY-NEXT:  30398 f0ffffff ffffffff

# NOAPPLY:      Contents of section .got:
# NOAPPLY-NEXT:  20338 00000000 00000000 00000000 00000000
# NOAPPLY-NEXT:  20348 00000000 00000000 00000000 00000000
# NOAPPLY-NEXT:  20358 00000000 00000000 00000000 00000000
# NOAPPLY-NEXT:  20368 08000000 00000000 00000000 00000000
# NOAPPLY:      Contents of section .data:
# NOAPPLY-NEXT:  30378 00000000 00000000 00000000 00000000
# NOAPPLY-NEXT:  30388 00000000 00000000 00000000 00000000
# NOAPPLY-NEXT:  30398 00000000 00000000

## A PIE against it: a COPY (var), GLOB_DAT and JUMP_SLOT (f), TPOFF64 (tg),
## a dynamic ABS64 with a negative addend (f - 8).
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj main.s -o main.o
# RUN: ld.lld -pie --apply-dynamic-relocs --check-dynamic-relocations --allow-shlib-undefined main.o noapply.so -o main
# RUN: llvm-readelf -r main | FileCheck %s --check-prefix=PIE
# RUN: llvm-objdump -s -j .data main | FileCheck %s --check-prefix=PIEDATA
# PIE-DAG: R_KLAUSSCPU_COPY {{.*}} var + 0
# PIE-DAG: R_KLAUSSCPU_ABS64 {{.*}} f - 8
# PIE-DAG: R_KLAUSSCPU_TPOFF64 {{.*}} tg + 0
# PIE-DAG: R_KLAUSSCPU_GLOB_DAT {{.*}} f + 0
# PIE-DAG: R_KLAUSSCPU_JUMP_SLOT {{.*}} f + 0
# PIEDATA:      Contents of section .data:
# PIEDATA-NEXT: f8ffffff ffffffff

#--- a.s
        .text
        .globl f
f:      leapc r1, %gotpcrel(loc)
        leapc r1, %gotpcrel(var)
        leapc r0, %tlsgd(tl)
        leapc r0, %tlsgd(tg)
        leapc r1, %gottprel(tl)
        leapc r1, %gottprel(tg)
        ret
        .data
        .p2align 3
        .globl var
        .type var, @object
        .size var, 8
loc:    .quad 0
var:    .quad 0
        .quad var - 8
        .quad loc - 0x100000
        .quad ext - 16
        .section .tdata,"awT",@progbits
        .p2align 3
        .globl tg
        .hidden tl
pad:    .quad 0
tl:     .quad 1
tg:     .quad 2

#--- main.s
        .globl _start
_start: leapc r1, var                   # COPY
        leapc r1, %gotpcrel(f)
        leapc r1, %gottprel(tg)
        callrel f
        ret
        .data
        .p2align 3
        .quad f - 8
