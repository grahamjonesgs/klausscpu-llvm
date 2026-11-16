# RUN: llvm-mc -triple=klausscpu -filetype=obj %s -o %t.o
# RUN: llvm-readobj -r %t.o | FileCheck %s
# RUN: llvm-readelf -S %t.o | FileCheck %s --check-prefix=SEC
# RUN: llvm-mc -triple=klausscpu -filetype=obj %s --defsym=NONONE=1 -o %t0.o
# RUN: llvm-objcopy -O binary -j .text %t.o %t.text
# RUN: llvm-objcopy -O binary -j .text %t0.o %t0.text
# RUN: cmp %t.text %t0.text
# RUN: not llvm-mc -triple=klausscpu -filetype=obj %s --defsym=ERR=1 -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR

# .reloc with BFD_RELOC_NONE gives R_KLAUSSCPU_NONE (it used to be "unknown
# relocation name").  .cg_profile, which clang emits for every PGO build
# (-fprofile-use), needs it: the integrated assembler keeps each profiled
# symbol alive in the symbol table with an R_*_NONE against it, which lld
# reads (with the .llvm.call-graph-profile section) for its call-graph sort.

# A NONE relocation writes no bytes, even on an instruction with a real
# fixup of its own (h's word 1, an ABS32): .text is the same with and
# without the .reloc lines (NONONE).

        .text
        .globl f, g, h
f:      ret
g:      ret
h:      setr r1, ext
.ifndef NONONE
        .reloc ., BFD_RELOC_NONE, f
        .reloc g+4, BFD_RELOC_NONE, g
        .reloc h+4, BFD_RELOC_NONE, f
        .cg_profile f, g, 42
.endif

.ifdef ERR
        .reloc ., BFD_RELOC_32, f
# ERR: error: unknown relocation name
.endif

# CHECK:      Section ({{.*}}) .rela.text {
# CHECK-NEXT:   0xC R_KLAUSSCPU_ABS32 ext 0x0
# CHECK-NEXT:   0x10 R_KLAUSSCPU_NONE f 0x0
# CHECK-NEXT:   0x8 R_KLAUSSCPU_NONE g 0x0
# CHECK-NEXT:   0xC R_KLAUSSCPU_NONE f 0x0
# CHECK-NEXT: }
# One entry: both its symbols' relocations at its offset (REL, no addend).
# CHECK:      Section ({{.*}}) .rel.llvm.call-graph-profile {
# CHECK-NEXT:   0x0 R_KLAUSSCPU_NONE f{{$}}
# CHECK-NEXT:   0x0 R_KLAUSSCPU_NONE g{{$}}
# CHECK-NEXT: }

# SEC: .llvm.call-graph-profile
