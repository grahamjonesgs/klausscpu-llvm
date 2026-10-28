# REQUIRES: klausscpu
## Each R_KLAUSSCPU_CALL18 thunk (one per symbol and addend) has its own
## name.  A call to an absolute symbol that the assembler resolved has no
## symbol, only the address in the addend: its thunk is named after the
## address (it used to be `__KlaussCPUThunk_` for every such target).  A
## named callee with an addend gets the addend in the name.

# RUN: llvm-mc -triple=klausscpu -filetype=obj %s -o %t.o
# RUN: llvm-readobj -r %t.o | FileCheck %s --check-prefix=REL
# RUN: ld.lld -Ttext=0x10000 %t.o --defsym=ext=0x700000 -o %t
# RUN: llvm-objdump -d --no-show-raw-insn %t | FileCheck %s

# REL:      0x0 R_KLAUSSCPU_CALL18 - 0x400000
# REL-NEXT: 0x4 R_KLAUSSCPU_CALL18 - 0x500000
# REL-NEXT: 0x8 R_KLAUSSCPU_CALL18 - 0x400008
# REL-NEXT: 0xC R_KLAUSSCPU_CALL18 - 0x600000

# CHECK:      <_start>:
# CHECK-NEXT: callrel 0x[[#%x,A:]] <__KlaussCPUThunk_0x400000>
# CHECK-NEXT: callrel 0x[[#%x,B:]] <__KlaussCPUThunk_0x500000>
# CHECK-NEXT: callrel 0x[[#%x,C:]] <__KlaussCPUThunk_0x400008>
# CHECK-NEXT: callrel 0x[[#%x,D:]] <__KlaussCPUThunk_0x600000>
# CHECK-NEXT: callrel 0x[[#%x,E:]] <__KlaussCPUThunk_ext>
# CHECK-NEXT: callrel 0x[[#%x,F:]] <__KlaussCPUThunk_ext+0x8>
# CHECK-NEXT: callrel 0x[[#%x,G:]] <__KlaussCPUThunk_ext-0x4>
# CHECK-NEXT: halt

# CHECK:      [[#A]] <__KlaussCPUThunk_0x400000>:
# CHECK-NEXT: jmprel 0x400000
# CHECK:      [[#B]] <__KlaussCPUThunk_0x500000>:
# CHECK-NEXT: jmprel 0x500000
# CHECK:      [[#C]] <__KlaussCPUThunk_0x400008>:
# CHECK-NEXT: jmprel 0x400008
# CHECK:      [[#D]] <__KlaussCPUThunk_0x600000>:
# CHECK-NEXT: jmprel 0x600000
# CHECK:      [[#E]] <__KlaussCPUThunk_ext>:
# CHECK-NEXT: jmprel 0x700000
# CHECK:      [[#F]] <__KlaussCPUThunk_ext+0x8>:
# CHECK-NEXT: jmprel 0x700008
# CHECK:      [[#G]] <__KlaussCPUThunk_ext-0x4>:
# CHECK-NEXT: jmprel 0x6ffffc

        .text
        .globl _start
_start:
        callrel abs_a
        callrel abs_b
        callrel abs_a+8
        callrel gabs
        callrel ext
        callrel ext+8
        callrel ext-4
        halt

        .set abs_a, 0x400000
        .set abs_b, 0x500000
        .globl gabs
        .set gabs, 0x600000
