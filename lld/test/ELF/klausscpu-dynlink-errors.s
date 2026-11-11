# REQUIRES: klausscpu
## What ABI.md "Dynamic linking" excludes: RELR (ELF32 RELR describes
## 4-byte words, the dynamic fields are 8 bytes), Android's packed
## relocations (no DT_RELA, which musl reads), REL, and IFUNC (musl has none,
## and there is no R_KLAUSSCPU_IRELATIVE).  Static links and -r are as before:
## the packing options do nothing there, and an IFUNC definition nothing
## refers to links.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj a.s -o a.o
# RUN: not ld.lld -shared -z pack-relative-relocs a.o -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=RELR
# RUN: not ld.lld -pie --pack-dyn-relocs=relr a.o -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=RELR
# RELR: error: -z pack-relative-relocs and --pack-dyn-relocs=relr are not supported for KlaussCPU
## -z nopack-relative-relocs is fine, and so is either packing in a static
## link.
# RUN: ld.lld -shared -z nopack-relative-relocs a.o -o /dev/null
# RUN: ld.lld -static -z pack-relative-relocs a.o -o /dev/null
# RUN: ld.lld --pack-dyn-relocs=relr a.o -o /dev/null
# RUN: ld.lld --pack-dyn-relocs=android a.o -o /dev/null
# RUN: not ld.lld -shared --pack-dyn-relocs=android a.o -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=ANDROID
# RUN: not ld.lld -pie --pack-dyn-relocs=android+relr a.o -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefixes=RELR,ANDROID
# ANDROID: error: --pack-dyn-relocs=android is not supported for KlaussCPU
## Dynamic relocations are RELA.
# RUN: not ld.lld -shared -z rel a.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=REL
# REL: error: -z rel is not supported for KlaussCPU shared libraries and PIE

# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj ifunc.s -o ifunc.o
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj ifuncdef.s -o ifuncdef.o
## A reference is an error in any link; a definition in a shared library or
## PIE.
# RUN: not ld.lld -static ifunc.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=IFUNCREF --implicit-check-not=error:
# RUN: not ld.lld -shared ifunc.o -o /dev/null 2>&1 | FileCheck %s --check-prefixes=IFUNC,IFUNCREF
# RUN: not ld.lld -pie ifuncdef.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=IFUNC
# IFUNC: error: {{.*}}.o: STT_GNU_IFUNC symbol ifn is not supported for KlaussCPU
# IFUNCREF: error: ifunc.o:(.text+0x4): relocation R_KLAUSSCPU_CALL18 against STT_GNU_IFUNC symbol ifn is not supported for KlaussCPU
## An unreferenced definition in a static link or -r.
# RUN: ld.lld -static ifuncdef.o -o /dev/null
# RUN: ld.lld -r ifuncdef.o -o /dev/null

#--- a.s
        .data
        .p2align 3
x:      .quad x

#--- ifunc.s
        .text
        .globl ifn, _start
        .type ifn, @gnu_indirect_function
ifn:    ret
_start: callrel ifn

#--- ifuncdef.s
        .text
        .globl ifn, _start
        .type ifn, @gnu_indirect_function
ifn:    ret
_start: ret
