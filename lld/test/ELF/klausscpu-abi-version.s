# REQUIRES: klausscpu
## The low byte of e_flags is the calling convention the code follows
## (KlaussCPUABI::ABIVersion): the assembler writes 3, the version with
## __int128 8-aligned.  Version 2 has it 16-aligned (and, like 3, sign-extended
## i32 arguments, aggregates in registers and the vararg home area only in
## variadic calls); code built before that has 0.  lld refuses to
## mix them, so an object an older toolchain compiled cannot be linked in by
## mistake; an object without code (data, -b binary) is not checked.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -triple=klausscpu -filetype=obj main.s -o main.o
# RUN: llvm-readobj -h main.o | FileCheck %s --check-prefix=OBJ
# RUN: yaml2obj old.yaml -o old.o
## An ABI 2 object (__int128 16-aligned): code with e_flags 2 (the
## low byte of e_flags is at offset 36 of an ELF32 header).
# RUN: llvm-mc -triple=klausscpu -filetype=obj v2.s -o v2src.o
# RUN: %python -c "b = bytearray(open('v2src.o', 'rb').read()); b[36] = 2; open('v2.o', 'wb').write(b)"
# RUN: yaml2obj data.yaml -o data.o
# RUN: ld.lld main.o data.o -o main
# RUN: llvm-readobj -h main | FileCheck %s --check-prefix=EXE
# RUN: not ld.lld main.o old.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR
# RUN: not ld.lld main.o v2.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR2
# RUN: not ld.lld v2.o main.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR2R
# RUN: ld.lld old.o data.o -o old
# RUN: llvm-readobj -h old | FileCheck %s --check-prefix=OLD
## Shared libraries are checked by the same rule: a shared library with
## code of ABI 2 is refused, one without code (data only) is not.  A shared
## library never chooses the output's version: without code in the objects
## it is the current one, 3.
# RUN: ld.lld -shared main.o -o main.so
# RUN: llvm-readobj -h main.so | FileCheck %s --check-prefix=EXE
# RUN: ld.lld -shared v2.o -o v2.so
# RUN: not ld.lld main.o v2.so -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERRSO
# RUN: not ld.lld -pie main.o v2.so -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERRSO
# RUN: ld.lld -shared data.o -o data.so
# RUN: ld.lld -pie main.o data.so main.so -o pie
# RUN: llvm-readobj -h pie | FileCheck %s --check-prefix=EXE
# RUN: not ld.lld -shared data.o v2.so -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERRSO3
# RUN: ld.lld -shared data.o main.so -o data2.so
# RUN: llvm-readobj -h data2.so | FileCheck %s --check-prefix=EXE

# OBJ: Flags [ (0x3)
# EXE: Flags [ (0x3)
# ERR: error: old.o: KlaussCPU ABI version 0 cannot be linked with ABI version 3 in main.o; rebuild it with the current toolchain
# ERR2: error: v2.o: KlaussCPU ABI version 2 cannot be linked with ABI version 3 in main.o; rebuild it with the current toolchain
# ERR2R: error: main.o: KlaussCPU ABI version 3 cannot be linked with ABI version 2 in v2.o; rebuild it with the current toolchain
# OLD: Flags [ (0x0)
# ERRSO: error: v2.so: KlaussCPU ABI version 2 cannot be linked with ABI version 3 in main.o; rebuild it with the current toolchain
# ERRSO3: error: v2.so: KlaussCPU ABI version 2 cannot be linked with ABI version 3; rebuild it with the current toolchain

#--- main.s
  .globl _start
_start:
  ret

#--- v2.s
  .globl v2func
v2func:
  ret

#--- old.yaml
## An object from an older toolchain: code, e_flags 0.
--- !ELF
FileHeader:
  Class:   ELFCLASS32
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_KLAUSSCPU
Sections:
  - Name:    .text.old
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: '00008065'

#--- data.yaml
## Data only, e_flags 0.
--- !ELF
FileHeader:
  Class:   ELFCLASS32
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_KLAUSSCPU
Sections:
  - Name:    .rodata.blob
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC ]
    Content: '01020304'
