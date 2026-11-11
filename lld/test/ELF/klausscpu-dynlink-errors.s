# REQUIRES: klausscpu
## What ABI.md "Dynamic linking" excludes: RELR (ELF32 RELR describes
## 4-byte words, the dynamic fields are 8 bytes), REL, and IFUNC (musl has
## none, and there is no R_KLAUSSCPU_IRELATIVE; in any link).

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj a.s -o a.o
# RUN: not ld.lld -shared -z pack-relative-relocs a.o -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=RELR
# RUN: not ld.lld -pie --pack-dyn-relocs=relr a.o -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=RELR
# RELR: error: -z pack-relative-relocs and --pack-dyn-relocs=relr are not supported for KlaussCPU
## -z nopack-relative-relocs is fine.
# RUN: ld.lld -shared -z nopack-relative-relocs a.o -o /dev/null
## Dynamic relocations are RELA.
# RUN: not ld.lld -shared -z rel a.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=REL
# REL: error: -z rel is not supported for KlaussCPU shared libraries and PIE

# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj ifunc.s -o ifunc.o
# RUN: not ld.lld -static ifunc.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=IFUNC
# RUN: not ld.lld -shared ifunc.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=IFUNC
# IFUNC: error: ifunc.o: STT_GNU_IFUNC symbol ifn is not supported for KlaussCPU
# IFUNC: error: ifunc.o:(.text+0x4): relocation R_KLAUSSCPU_CALL18 against STT_GNU_IFUNC symbol ifn is not supported for KlaussCPU

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
