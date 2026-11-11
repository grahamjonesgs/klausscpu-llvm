# REQUIRES: klausscpu
## A shared library (ABI.md "Dynamic linking"): GOT entries of 8 bytes
## (GLOB_DAT for a preemptible symbol, RELATIVE for one that cannot be
## preempted, the value itself for an absolute one), 16-byte PLT entries
## without a header that load their .got.plt slot (JUMP_SLOT, zero in the
## file), calls (CALL18 and PLT32) to a preemptible function through its PLT
## entry and to a hidden one directly, ABS64 data as RELATIVE or a dynamic
## ABS64, 64 KiB pages and -z now by default.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj a.s -o a.o
# RUN: ld.lld -shared -soname liba.so a.o --defsym=abs=0xf0000000 -o a.so
# RUN: llvm-readelf -h -S -l -d -r --dyn-syms a.so | FileCheck %s
# RUN: llvm-objdump -d --no-show-raw-insn a.so | FileCheck %s --check-prefix=DIS
# RUN: llvm-objdump -d -j .plt a.so | FileCheck %s --check-prefix=PLT
# RUN: llvm-objdump -s -j .got -j .got.plt -j .data a.so | FileCheck %s --check-prefix=DATA

# CHECK:      Class:   ELF32
# CHECK:      Type:    DYN (Shared object file)
# CHECK:      Machine: KlaussCPU
# CHECK:      Flags:   0x3

## .plt: 16-aligned, sh_entsize 16.  .got and .got.plt: 8-aligned, no
## header entries: three GOT entries (var, abs, local) and three .got.plt
## slots.
# CHECK:      Name      Type     Address  Off    Size   ES Flg Lk Inf Al
# CHECK:      .rela.dyn RELA     00000210 000210 000054 0c   A  1   0  4
# CHECK-NEXT: .rela.plt RELA     00000264 000264 000024 0c  AI  1  11  4
# CHECK-NEXT: .text     PROGBITS 00010288 000288 000050 00  AX  0   0  4
# CHECK-NEXT: .plt      PROGBITS 000102e0 0002e0 000030 10  AX  0   0 16
# CHECK-NEXT: .dynamic  DYNAMIC  00020310 000310 000090 08  WA  4   0  4
# CHECK-NEXT: .got      PROGBITS 000203a0 0003a0 000018 00  WA  0   0  8
# CHECK-NEXT: .got.plt  PROGBITS 000203b8 0003b8 000018 00  WA  0   0  8
# CHECK-NEXT: .relro_padding NOBITS 000203d0 0003d0 00fc30 00 WA 0  0  1
# CHECK-NEXT: .data     PROGBITS 000303d0 0003d0 000040 00  WA  0   0  8

## 64 KiB pages; .got.plt (-z now) in PT_GNU_RELRO, which ends on a page.
# CHECK:      Type      Offset   VirtAddr   PhysAddr   FileSiz MemSiz  Flg Align
# CHECK-NEXT: PHDR
# CHECK-NEXT: LOAD      0x000000 0x00000000 0x00000000 0x00288 0x00288 R   0x10000
# CHECK-NEXT: LOAD      0x000288 0x00010288 0x00010288 0x00088 0x00088 R E 0x10000
# CHECK-NEXT: LOAD      0x000310 0x00020310 0x00020310 0x000c0 0x0fcf0 RW  0x10000
# CHECK-NEXT: LOAD      0x0003d0 0x000303d0 0x000303d0 0x00040 0x00040 RW  0x10000
# CHECK-NEXT: DYNAMIC   0x000310 0x00020310 0x00020310 0x00090 0x00090 RW  0x4
# CHECK-NEXT: GNU_RELRO 0x000310 0x00020310 0x00020310 0x000c0 0x0fcf0 R   0x1
# CHECK-NEXT: GNU_STACK 0x000000 0x00000000 0x00000000 0x00000 0x00000 RW  0x0
# CHECK:      06 .dynamic .got .got.plt .relro_padding

## No DT_TEXTREL, no DT_DEBUG in a shared library.  DT_PLTGOT is .got.plt.
# CHECK:      Dynamic section at offset 0x310 contains 18 entries:
# CHECK-NEXT: Tag        Type        Name/Value
# CHECK-NEXT: (SONAME)    Library soname: [liba.so]
# CHECK-NEXT: (FLAGS)     BIND_NOW
# CHECK-NEXT: (FLAGS_1)   NOW
# CHECK-NEXT: (RELA)      0x210
# CHECK-NEXT: (RELASZ)    84 (bytes)
# CHECK-NEXT: (RELAENT)   12 (bytes)
# CHECK-NEXT: (RELACOUNT) 3
# CHECK-NEXT: (JMPREL)    0x264
# CHECK-NEXT: (PLTRELSZ)  36 (bytes)
# CHECK-NEXT: (PLTGOT)    0x203b8
# CHECK-NEXT: (PLTREL)    RELA
# CHECK-NEXT: (SYMTAB)    0x134
# CHECK-NEXT: (SYMENT)    16 (bytes)
# CHECK-NEXT: (STRTAB)    0x1f4
# CHECK-NEXT: (STRSZ)     26 (bytes)
# CHECK-NEXT: (GNU_HASH)  0x194
# CHECK-NEXT: (HASH)      0x1bc
# CHECK-NEXT: (NULL)      0x0

## RELATIVE for local's GOT entry, &f2 (hidden) and &local + 8; ABS64 for
## the preemptible f and var (a negative addend) and the undefined ext;
## GLOB_DAT for var; JUMP_SLOT for g (default visibility, so preemptible
## though defined here), ext and ext2.  The absolute hidden abs needs none.
# CHECK:      Relocation section '.rela.dyn' at offset 0x210 contains 7 entries:
# CHECK-NEXT: Offset     Info    Type                Sym. Value  Symbol's Name + Addend
# CHECK-NEXT: 000203b0  0000000f R_KLAUSSCPU_RELATIVE              303d8
# CHECK-NEXT: 000303f0  0000000f R_KLAUSSCPU_RELATIVE              102d4
# CHECK-NEXT: 000303f8  0000000f R_KLAUSSCPU_RELATIVE              303e0
# CHECK-NEXT: 00030408  00000102 R_KLAUSSCPU_ABS64      00000000   ext + 0
# CHECK-NEXT: 000303e8  00000302 R_KLAUSSCPU_ABS64      00010288   f + 0
# CHECK-NEXT: 000203a0  0000040d R_KLAUSSCPU_GLOB_DAT   000303d0   var + 0
# CHECK-NEXT: 00030400  00000402 R_KLAUSSCPU_ABS64      000303d0   var - 8
# CHECK:      Relocation section '.rela.plt' at offset 0x264 contains 3 entries:
# CHECK-NEXT: Offset     Info    Type                Sym. Value  Symbol's Name + Addend
# CHECK-NEXT: 000203b8  0000050e R_KLAUSSCPU_JUMP_SLOT  000102d0   g + 0
# CHECK-NEXT: 000203c0  0000010e R_KLAUSSCPU_JUMP_SLOT  00000000   ext + 0
# CHECK-NEXT: 000203c8  0000020e R_KLAUSSCPU_JUMP_SLOT  00000000   ext2 + 0

# CHECK:      Symbol table '.dynsym' contains 6 entries:
# CHECK:      1: 00000000     0 NOTYPE  GLOBAL DEFAULT   UND ext
# CHECK-NEXT: 2: 00000000     0 NOTYPE  GLOBAL DEFAULT   UND ext2
# CHECK-NEXT: 3: 00010288     0 FUNC    GLOBAL DEFAULT     7 f
# CHECK-NEXT: 4: 000303d0     8 OBJECT  GLOBAL DEFAULT    13 var
# CHECK-NEXT: 5: 000102d0     0 FUNC    GLOBAL DEFAULT     7 g

## GOT entries: var 0x203a0, abs 0x203a8, local 0x203b0.  The .got.plt
## slots of g, ext, ext2: 0x203b8, 0x203c0, 0x203c8; their PLT entries
## 0x102e0, 0x102f0, 0x10300.
# DIS:      <f>:
# DIS-NEXT:   leapc r1, 0x203a0
# DIS-NEXT:   ldidx64 r1, r1, 0
# DIS-NEXT:   leapc r2, 0x203b0
# DIS-NEXT:   ldidx64 r2, r2, 0
# DIS-NEXT:   leapc r3, 0x203a8
# DIS-NEXT:   ldidx64 r3, r3, 0
# DIS-NEXT:   callrel 0x102e0
# DIS-NEXT:   callrel 0x102f0
# DIS-NEXT:   callrel 0x10300
# DIS-NEXT:   jmprel 0x102f0
# DIS-NEXT:   jmprel 0x102e0
# DIS-NEXT:   callrel 0x102d4 <f2>
# DIS-NEXT:   jmprel 0x102d4 <f2>
# DIS-EMPTY:
# DIS-NEXT: <g>:

## Each entry: leapc r14, slot - entry; ldidx64 r14, r14, 0; jmpr r14.
## 0x203b8 - 0x102e0 = 0x100d8.
# PLT:      000102e0 <.plt>:
# PLT-NEXT: 102e0: 00 0e 80 8b d8 00 01 00 leapc r14, 0x203b8
# PLT-NEXT: 102e8: e0 0e 30 5b             ldidx64 r14, r14, 0
# PLT-NEXT: 102ec: 0e 00 80 60             jmpr r14
# PLT-NEXT: 102f0: 00 0e 80 8b d0 00 01 00 leapc r14, 0x203c0
# PLT-NEXT: 102f8: e0 0e 30 5b             ldidx64 r14, r14, 0
# PLT-NEXT: 102fc: 0e 00 80 60             jmpr r14
# PLT-NEXT: 10300: 00 0e 80 8b c8 00 01 00 leapc r14, 0x203c8
# PLT-NEXT: 10308: e0 0e 30 5b             ldidx64 r14, r14, 0
# PLT-NEXT: 1030c: 0e 00 80 60             jmpr r14

## The file holds 0 where a dynamic relocation writes (RELA), and the
## absolute symbol's value in its GOT entry, zero-extended.
# DATA:      Contents of section .got:
# DATA-NEXT:  203a0 00000000 00000000 000000f0 00000000
# DATA-NEXT:  203b0 00000000 00000000
# DATA-NEXT: Contents of section .got.plt:
# DATA-NEXT:  203b8 00000000 00000000 00000000 00000000
# DATA-NEXT:  203c8 00000000 00000000
# DATA-NEXT: Contents of section .data:
# DATA-NEXT:  303d0 00000000 00000000 05000000 00000000
# DATA-NEXT:  303e0 00000000 00000000 00000000 00000000
# DATA-NEXT:  303f0 00000000 00000000 00000000 00000000
# DATA-NEXT:  30400 00000000 00000000 00000000 00000000

## -z lazy turns BIND_NOW off (the PLT stays the same); explicit page sizes
## override the 64 KiB defaults.
# RUN: ld.lld -shared -z lazy -z max-page-size=0x1000 -z common-page-size=0x1000 a.o --defsym=abs=0xf0000000 -o lazy.so
# RUN: llvm-readelf -l -d lazy.so | FileCheck %s --check-prefix=LAZY
# LAZY:     LOAD 0x000000 0x00000000 0x00000000 0x{{[0-9a-f]+}} 0x{{[0-9a-f]+}} R 0x1000
# LAZY-NOT: BIND_NOW
# LAZY-NOT: NOW

## Errors: 32-bit and PC-relative references to a preemptible symbol (a
## 2-word jump too: only CALL18 and PLT32 go through the PLT), an ABS32
## against any symbol that is not absolute, and a dynamic relocation in a
## read-only section have no dynamic form.
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj err.s -o err.o
# RUN: not ld.lld -shared err.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR
# ERR:      error: relocation R_KLAUSSCPU_PCREL32 cannot be used against symbol 'pre'; recompile with -fPIC
# ERR-NEXT: >>> defined in err.o
# ERR-NEXT: >>> referenced by err.o:(.text+0x4)
# ERR:      error: relocation R_KLAUSSCPU_PCREL32 cannot be used against symbol 'ext'; recompile with -fPIC
# ERR:      error: relocation R_KLAUSSCPU_PCREL32 cannot be used against symbol 'pre'; recompile with -fPIC
# ERR-NEXT: >>> defined in err.o
# ERR-NEXT: >>> referenced by err.o:(.text+0x14)
# ERR:      error: relocation R_KLAUSSCPU_ABS32 cannot be used against symbol 'pre'; recompile with -fPIC
# ERR:      error: relocation R_KLAUSSCPU_ABS32 cannot be used against local symbol; recompile with -fPIC
# ERR:      error: relocation R_KLAUSSCPU_PC32 cannot be used against symbol 'pre'; recompile with -fPIC
# ERR:      error: relocation R_KLAUSSCPU_PC64 cannot be used against symbol 'pre'; recompile with -fPIC
# ERR:      error: relocation R_KLAUSSCPU_ABS64 cannot be used against symbol 'pre'; recompile with -fPIC
# ERR-NEXT: >>> defined in err.o
# ERR-NEXT: >>> referenced by err.o:(.rodata+0x10)
# ERR:      error: relocation R_KLAUSSCPU_ABS64 cannot be used against local symbol; recompile with -fPIC
# ERR-NEXT: >>> defined in err.o
# ERR-NEXT: >>> referenced by err.o:(.rodata+0x18)

#--- a.s
        .text
        .globl f
        .type f, @function
f:
        leapc   r1, %gotpcrel(var)
        ldidx64 r1, r1, 0
        leapc   r2, %gotpcrel(local)
        ldidx64 r2, r2, 0
        leapc   r3, %gotpcrel(abs)
        ldidx64 r3, r3, 0
        callrel g
        callrel ext
        callrel %plt(ext2)
        tail    %plt(ext)
        tail    g
        callrel f2
        tail    f2

        .globl g
        .type g, @function
g:      ret
        .globl f2
        .hidden f2
        .type f2, @function
f2:     ret

        .data
        .p2align 3
        .globl var
        .type var, @object
        .size var, 8
var:    .quad 0
local:  .quad 5
        .quad 0
        .quad f
        .quad f2
        .quad local + 8
        .quad var - 8
        .quad ext

        .hidden abs

#--- err.s
        .text
        .globl pre
        .type pre, @function
pre:
        leapc   r1, pre
        leapc   r2, ext
        jmprel  pre
        .data
        .long   pre
        .long   loc
        .long   pre - .
        .quad   pre - .
loc:
        .section .rodata,"a",@progbits
        .quad   0
        .quad   0
        .quad   pre
        .quad   loc
