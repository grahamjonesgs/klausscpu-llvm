# REQUIRES: klausscpu
## TLS initial exec (R_KLAUSSCPU_TLS_GOTTPREL32: an 8-byte GOT entry with
## the TP offset) and general dynamic (R_KLAUSSCPU_TLS_GD32: a GOT pair
## {module ID, offset} for __tls_get_addr), ABI.md "Dynamic linking".  Neither
## is relaxed.  In a shared library the dynamic linker fills them (TPOFF64,
## DTPMOD64, DTPOFF64; symbol 0 for a symbol that cannot be preempted, the
## offset in the module's TLS segment as the addend or in the file), and IE
## sets DF_STATIC_TLS.  In an executable, static or PIE, lld fills them for
## its own TLS: the TP offset (variant II, negative, sign-extended to 64
## bits), module 1 and the offset; a library's TLS gets dynamic relocations.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj a.s -o a.o
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj gd.s -o gd.o
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj tga.s -o tga.o

## Shared library.  TLS segment: pad 0, gvar 8, ievar 0x10, lvar 0x18.
# RUN: ld.lld -shared a.o -o a.so
# RUN: llvm-readelf -l -d -r a.so | FileCheck %s --check-prefix=SO
# RUN: llvm-objdump -d --no-show-raw-insn --no-leading-addr a.so | FileCheck %s --check-prefix=SODIS
# RUN: llvm-objdump -s -j .got a.so | FileCheck %s --check-prefix=SOGOT

# SO:      TLS {{.*}} 0x00020 0x00020 R 0x8
# SO:      (FLAGS) BIND_NOW STATIC_TLS
# SO:      Relocation section '.rela.dyn' at offset {{.*}} contains 5 entries:
# SO-NEXT: Offset     Info    Type                Sym. Value  Symbol's Name + Addend
# SO-NEXT: 00020340  00000011 R_KLAUSSCPU_DTPMOD64              0
# SO-NEXT: 00020350  00000010 R_KLAUSSCPU_TPOFF64               18
# SO-NEXT: 00020330  00000311 R_KLAUSSCPU_DTPMOD64   00000008   gvar + 0
# SO-NEXT: 00020338  00000312 R_KLAUSSCPU_DTPOFF64   00000008   gvar + 0
# SO-NEXT: 00020358  00000410 R_KLAUSSCPU_TPOFF64    00000010   ievar + 0
# SO:      Relocation section '.rela.plt' at offset {{.*}} contains 1 entries:
# SO:      R_KLAUSSCPU_JUMP_SLOT  00000000   __tls_get_addr + 0

## GD pairs: gvar 0x20330, lvar 0x20340; IE entries: lvar 0x20350, ievar
## 0x20358.  __tls_get_addr is called through its PLT entry.
# SODIS:      <tf>:
# SODIS-NEXT:   leapc r0, 0x20330
# SODIS-NEXT:   callrel 0x10280
# SODIS-NEXT:   leapc r0, 0x20340
# SODIS-NEXT:   callrel 0x10280
# SODIS-NEXT:   leapc r1, 0x20358
# SODIS-NEXT:   ldidx64 r1, r1, 0
# SODIS-NEXT:   leapc r1, 0x20350
# SODIS-NEXT:   ldidx64 r1, r1, 0

## lvar's DTP offset (0x18) is written by lld into the second entry of its
## pair; everything else is 0 for the dynamic linker.
# SOGOT:      20330 00000000 00000000 00000000 00000000
# SOGOT-NEXT: 20340 00000000 00000000 18000000 00000000
# SOGOT-NEXT: 20350 00000000 00000000 00000000 00000000

## Only general dynamic: no DF_STATIC_TLS.
# RUN: ld.lld -shared gd.o -o gd.so
# RUN: llvm-readelf -d gd.so | FileCheck %s --check-prefix=GDSO
# GDSO:     (FLAGS) BIND_NOW
# GDSO-NOT: STATIC_TLS

## Static executable: no dynamic relocations, the GOT filled by lld:
## module 1 + offset (8, 0x18) for the GD pairs, TP offsets for IE:
## p_memsz 0x20, p_align 8: lvar 0x18 - 0x20 = -8, ievar 0x10 - 0x20 = -16.
# RUN: ld.lld -static a.o tga.o -o static
# RUN: llvm-readelf -l -r static | FileCheck %s --check-prefix=EXE
# RUN: llvm-objdump -s -j .got static | FileCheck %s --check-prefix=EXEGOT
# EXE:     LOAD {{.*}} 0x1000
# EXE:     TLS {{.*}} 0x00020 0x00020 R 0x8
# EXE:     There are no relocations in this file.
# EXEGOT:      01000000 00000000 08000000 00000000
# EXEGOT-NEXT: 01000000 00000000 18000000 00000000
# EXEGOT-NEXT: f8ffffff ffffffff f0ffffff ffffffff

## In a PIE, the same GOT, and no dynamic relocations for its own TLS.
# RUN: ld.lld -pie a.o tga.o -o pie
# RUN: llvm-readelf -d -r pie | FileCheck %s --check-prefix=PIE
# RUN: llvm-objdump -s -j .got pie | FileCheck %s --check-prefix=EXEGOT
# PIE-NOT: STATIC_TLS
# PIE:     There are no relocations in this file.

## A PIE using a library's TLS: TPOFF64 and DTPMOD64/DTPOFF64 against the
## symbol.
# RUN: ld.lld -shared -soname a.so a.o -o liba.so
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj use.s -o use.o
# RUN: ld.lld -pie use.o tga.o liba.so -o use
# RUN: llvm-readelf -r use | FileCheck %s --check-prefix=USE
# USE:      Relocation section '.rela.dyn' at offset {{.*}} contains 3 entries:
# USE-NEXT: Offset     Info    Type                Sym. Value  Symbol's Name + Addend
# USE-NEXT: {{.*}} R_KLAUSSCPU_DTPMOD64   00000000   gvar + 0
# USE-NEXT: {{.*}} R_KLAUSSCPU_DTPOFF64   00000000   gvar + 0
# USE-NEXT: {{.*}} R_KLAUSSCPU_TPOFF64    00000000   ievar + 0

## A GOT TLS relocation against a symbol another object defines outside TLS.
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj notls.s -o notls.o
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj data.s -o data.o
# RUN: not ld.lld -shared notls.o data.o a.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=NOTLS
# NOTLS: error: notls.o:(.text+0x4): relocation R_KLAUSSCPU_TLS_GOTTPREL32 against non-TLS symbol data
# NOTLS: error: notls.o:(.text+0xc): relocation R_KLAUSSCPU_TLS_GD32 against non-TLS symbol data
## And an address GOT entry for one that is.
# NOTLS: error: notls.o:(.text+0x14): relocation R_KLAUSSCPU_GOTPCREL32 against TLS symbol gvar

#--- a.s
        .text
        .globl tf
tf:
        leapc   r0, %tlsgd(gvar)
        callrel __tls_get_addr
        leapc   r0, %tlsgd(lvar)
        callrel __tls_get_addr
        leapc   r1, %gottprel(ievar)
        ldidx64 r1, r1, 0
        leapc   r1, %gottprel(lvar)
        ldidx64 r1, r1, 0
        ret
        .section .tdata,"awT",@progbits
        .p2align 3
        .globl gvar, ievar, lvar
        .type gvar, @object
        .type ievar, @object
        .hidden lvar
pad:    .quad 7
gvar:   .quad 1
ievar:  .quad 2
lvar:   .quad 3

#--- gd.s
        leapc   r0, %tlsgd(x)
        callrel __tls_get_addr
        .section .tbss,"awT",@nobits
        .globl x
x:      .quad 0

#--- tga.s
        .globl __tls_get_addr, _start
        .type __tls_get_addr, @function
__tls_get_addr:
_start:
        ret

#--- use.s
        leapc   r0, %tlsgd(gvar)
        callrel __tls_get_addr
        leapc   r1, %gottprel(ievar)
        ldidx64 r1, r1, 0

#--- notls.s
        leapc   r1, %gottprel(data)
        leapc   r1, %tlsgd(data)
        leapc   r1, %gotpcrel(gvar)

#--- data.s
        .data
        .globl data
data:   .quad 0
