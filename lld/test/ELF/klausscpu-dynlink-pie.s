# REQUIRES: klausscpu
## A PIE against a shared library (ABI.md "Dynamic linking"): PT_INTERP,
## DT_DEBUG, DF_1_PIE, -z now and 64 KiB pages by default; a short call
## (CALL18) to a library function through its PLT entry, through a jmprel
## thunk to that entry when it is out of reach; GOT entries (GLOB_DAT for a
## library symbol, RELATIVE for its own); and, for code that reached a
## library symbol directly, a COPY relocation (data) or a canonical PLT entry
## (function), neither of which is possible for a protected symbol.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj lib.s -o lib.o
# RUN: ld.lld -shared -soname libx.so lib.o -o libx.so
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj main.s -o main.o
# RUN: ld.lld -pie --dynamic-linker /lib/ld-musl-klausscpu.so.1 main.o libx.so -o main
# RUN: llvm-readelf -h -l -d -r --dyn-syms main | FileCheck %s
# RUN: llvm-objdump -d --no-show-raw-insn --no-leading-addr main | FileCheck %s --check-prefix=DIS
# RUN: llvm-objdump -s -j .got -j .got.plt -j .data main | FileCheck %s --check-prefix=DATA

# CHECK:      Type:    DYN (Shared object file)
# CHECK:      Entry point address: 0x1029c
# CHECK:      Flags:   0x3

# CHECK:      Type      Offset   VirtAddr   PhysAddr   FileSiz MemSiz  Flg Align
# CHECK-NEXT: PHDR      0x000034 0x00000034 0x00000034 0x00120 0x00120 R   0x4
# CHECK-NEXT: INTERP    0x000154 0x00000154 0x00000154 0x0001c 0x0001c R   0x1
# CHECK-NEXT: [Requesting program interpreter: /lib/ld-musl-klausscpu.so.1]
# CHECK-NEXT: LOAD      0x000000 0x00000000 0x00000000 0x0028c 0x0028c R   0x10000
# CHECK-NEXT: LOAD      0x00028c 0x0001028c 0x0001028c 0x80074 0x80074 R E 0x10000
# CHECK-NEXT: LOAD      0x080300 0x000a0300 0x000a0300 0x000b8 0x0fd00 RW  0x10000
# CHECK-NEXT: LOAD      0x0803b8 0x000b03b8 0x000b03b8 0x00018 0x00020 RW  0x10000
# CHECK-NEXT: DYNAMIC   0x080300 0x000a0300 0x000a0300 0x00098 0x00098 RW  0x4
# CHECK-NEXT: GNU_RELRO 0x080300 0x000a0300 0x000a0300 0x000b8 0x0fd00 R   0x1
# CHECK-NEXT: GNU_STACK 0x000000 0x00000000 0x00000000 0x00000 0x00000 RW  0x0
# CHECK:      04 .dynamic .got .got.plt .relro_padding
# CHECK-NEXT: 05 .data .bss

# CHECK:      (NEEDED)    Shared library: [libx.so]
# CHECK-NEXT: (FLAGS)     BIND_NOW
# CHECK-NEXT: (FLAGS_1)   NOW PIE
# CHECK-NEXT: (DEBUG)     0x0
# CHECK:      (PLTGOT)    0xa03a8

## obj: COPY into the PIE's .bss; fn: a canonical PLT entry (the st_value
## of its undefined .dynsym entry), which the ABS64 in .data names too;
## pobj (protected): its GOT entry; fn2: only called, an ordinary PLT entry
## (st_value 0); own: RELATIVE in the GOT and in .data.
# CHECK:      Relocation section '.rela.dyn' at offset 0x22c contains 6 entries:
# CHECK-NEXT: Offset     Info    Type                Sym. Value  Symbol's Name + Addend
# CHECK-NEXT: 000a0398  0000000f R_KLAUSSCPU_RELATIVE              102d4
# CHECK-NEXT: 000b03b8  0000000f R_KLAUSSCPU_RELATIVE              102d4
# CHECK-NEXT: 000a03a0  0000010d R_KLAUSSCPU_GLOB_DAT   00000000   pobj + 0
# CHECK-NEXT: 000b03c8  00000202 R_KLAUSSCPU_ABS64      00000000   fn2 + 8
# CHECK-NEXT: 000b03d0  0000030c R_KLAUSSCPU_COPY       000b03d0   obj + 0
# CHECK-NEXT: 000b03c0  00000402 R_KLAUSSCPU_ABS64      000902e0   fn + 0
# CHECK:      Relocation section '.rela.plt' at offset 0x274 contains 2 entries:
# CHECK-NEXT: Offset     Info    Type                Sym. Value  Symbol's Name + Addend
# CHECK-NEXT: 000a03a8  0000040e R_KLAUSSCPU_JUMP_SLOT  000902e0   fn + 0
# CHECK-NEXT: 000a03b0  0000020e R_KLAUSSCPU_JUMP_SLOT  00000000   fn2 + 0

# CHECK:      Symbol table '.dynsym' contains 5 entries:
# CHECK:      1: 00000000     0 OBJECT  GLOBAL DEFAULT   UND pobj
# CHECK-NEXT: 2: 00000000     0 FUNC    GLOBAL DEFAULT   UND fn2
# CHECK-NEXT: 3: 000b03d0     8 OBJECT  GLOBAL DEFAULT    15 obj
# CHECK-NEXT: 4: 000902e0     0 FUNC    GLOBAL DEFAULT   UND fn

## The PLT is more than 512 KiB from _start: its calls go through thunks to
## the PLT entries; near's call reaches the entry directly.
# DIS:      <__KlaussCPUThunk_fn>:
# DIS-NEXT:   jmprel 0x902e0
# DIS:      <__KlaussCPUThunk_fn2>:
# DIS-NEXT:   jmprel 0x902f0
# DIS:      <_start>:
# DIS-NEXT:   leapc r1, 0xb03d0
# DIS-NEXT:   leapc r2, 0x902e0
# DIS-NEXT:   callrel 0x1028c <__KlaussCPUThunk_fn>
# DIS-NEXT:   callrel 0x102d4 <own>
# DIS-NEXT:   leapc r3, 0xa03a0
# DIS-NEXT:   ldidx64 r3, r3, 0
# DIS-NEXT:   leapc r4, 0xa0398
# DIS-NEXT:   ldidx64 r4, r4, 0
# DIS-NEXT:   callrel 0x10294 <__KlaussCPUThunk_fn2>
# DIS-NEXT:   jmprel 0x10294 <__KlaussCPUThunk_fn2>
# DIS:      <near>:
# DIS-NEXT:   callrel 0x902f0
# DIS:      Disassembly of section .plt:
# DIS-EMPTY:
# DIS-NEXT: <.plt>:
# DIS-NEXT:   leapc r14, 0xa03a8
# DIS-NEXT:   ldidx64 r14, r14, 0
# DIS-NEXT:   jmpr r14
# DIS-NEXT:   leapc r14, 0xa03b0

# DATA:      Contents of section .got:
# DATA-NEXT:  a0398 00000000 00000000 00000000 00000000
# DATA-NEXT: Contents of section .got.plt:
# DATA-NEXT:  a03a8 00000000 00000000 00000000 00000000
# DATA-NEXT: Contents of section .data:
# DATA-NEXT:  b03b8 00000000 00000000 00000000 00000000
# DATA-NEXT:  b03c8 00000000 00000000

## The protected symbols cannot be preempted by a copy or a canonical PLT
## entry; nor can a TLS symbol of a library be reached by local exec.
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj bad.s -o bad.o
# RUN: not ld.lld -pie bad.o libx.so -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR
# ERR:      error: cannot preempt symbol: pobj
# ERR-NEXT: >>> defined in libx.so
# ERR-NEXT: >>> referenced by bad.o:(.text+0x4)
# ERR:      error: cannot preempt symbol: pfn
# ERR-NEXT: >>> defined in libx.so
# ERR-NEXT: >>> referenced by bad.o:(.text+0xc)
# ERR:      error: bad.o:(.text+0x18): relocation R_KLAUSSCPU_TPREL32 against tv defined in a shared library; recompile with -fPIE

#--- lib.s
        .text
        .globl fn, fn2, pfn
        .type fn, @function
fn:     ret
        .type fn2, @function
fn2:    ret
        .protected pfn
        .type pfn, @function
pfn:    ret
        .data
        .p2align 3
        .globl obj, pobj
        .type obj, @object
        .size obj, 8
obj:    .quad 1
        .protected pobj
        .type pobj, @object
        .size pobj, 8
pobj:   .quad 2
        .section .tdata,"awT",@progbits
        .globl tv
        .type tv, @object
        .size tv, 8
tv:     .quad 3

#--- main.s
        .text
        .globl _start, own
_start:
        leapc   r1, obj                 # PCREL32: COPY
        leapc   r2, fn                  # PCREL32: canonical PLT entry
        callrel fn                      # CALL18: the same entry
        callrel own                     # a PIE's definitions are local
        leapc   r3, %gotpcrel(pobj)
        ldidx64 r3, r3, 0
        leapc   r4, %gotpcrel(own)
        ldidx64 r4, r4, 0
        callrel fn2                     # out of reach: a thunk to the PLT
        tail    fn2
own:    ret
        .space  0x80000
near:   callrel fn2                     # in reach of the PLT
        ret
        .data
        .p2align 3
        .quad   own
        .quad   fn
        .quad   fn2 + 8

#--- bad.s
        .text
        .globl _start
_start:
        leapc   r1, pobj
        leapc   r1, pfn
        mfsr    r1, tp
        addi    r1, r1, %tprel(tv)
