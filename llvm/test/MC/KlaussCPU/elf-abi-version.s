# RUN: llvm-mc -triple=klausscpu -filetype=obj %s -o - | llvm-readobj -h - | FileCheck %s

## Every object records the calling convention its code follows in the low
## byte of e_flags (KlaussCPUABI::ABIVersion); lld refuses to link objects of
## different versions (lld/test/ELF/klausscpu-abi-version.s).

# CHECK: Flags [ (0x2)

  ret
