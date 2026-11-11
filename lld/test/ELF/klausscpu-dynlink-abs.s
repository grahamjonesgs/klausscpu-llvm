# REQUIRES: klausscpu
## Absolute relocations in a shared library or PIE (ABI.md "Dynamic
## linking").  There is no 32-bit dynamic relocation, so R_KLAUSSCPU_ABS32
## against any symbol that is not absolute is an error, and R_KLAUSSCPU_ABS64
## needs a writable field (or -z notext).  In a PIE neither may fall back on a
## copy relocation or a canonical PLT entry for a library symbol: the field
## would hold the link-time address, which does not move with the PIE.
## PC-relative references to a library symbol (PCREL32, PC32, PC64) and a
## writable ABS64 are fine.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj lib.s -o lib.o
# RUN: ld.lld -shared -soname libx.so lib.o -o libx.so
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj abs32.s -o abs32.o
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj ro.s -o ro.o
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj own.s -o own.o
# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj ok.s -o ok.o

## A PIE: ABS32 against a library function or object, in code or data.
# RUN: not ld.lld -pie abs32.o libx.so -o /dev/null 2>&1 | FileCheck %s --check-prefix=PIE32 --implicit-check-not=error:
# PIE32:      error: relocation R_KLAUSSCPU_ABS32 cannot be used against symbol 'lf'; recompile with -fPIE
# PIE32-NEXT: >>> defined in libx.so
# PIE32-NEXT: >>> referenced by abs32.o:(.text+0x4)
# PIE32:      error: relocation R_KLAUSSCPU_ABS32 cannot be used against symbol 'ldata'; recompile with -fPIE
# PIE32-NEXT: >>> defined in libx.so
# PIE32-NEXT: >>> referenced by abs32.o:(.text+0xc)
# PIE32:      error: relocation R_KLAUSSCPU_ABS32 cannot be used against symbol 'lf'; recompile with -fPIE
# PIE32-NEXT: >>> defined in libx.so
# PIE32-NEXT: >>> referenced by abs32.o:(.data+0x0)

## A PIE: ABS64 in a read-only section against a library symbol.
# RUN: not ld.lld -pie ro.o libx.so -o /dev/null 2>&1 | FileCheck %s --check-prefix=PIERO --implicit-check-not=error:
# PIERO:      error: relocation R_KLAUSSCPU_ABS64 cannot be used against symbol 'lf'; recompile with -fPIE
# PIERO-NEXT: >>> defined in libx.so
# PIERO-NEXT: >>> referenced by ro.o:(.rodata+0x0)
# PIERO:      error: relocation R_KLAUSSCPU_ABS64 cannot be used against symbol 'ldata'; recompile with -fPIE
# PIERO-NEXT: >>> defined in libx.so
# PIERO-NEXT: >>> referenced by ro.o:(.rodata+0x8)

## With -z notext the read-only field gets its dynamic ABS64 (DT_TEXTREL).
# RUN: ld.lld -pie -z notext ro.o libx.so -o notext 2>/dev/null
# RUN: llvm-readelf -d -r notext | FileCheck %s --check-prefix=NOTEXT
# NOTEXT:      (FLAGS) TEXTREL BIND_NOW
# NOTEXT:      R_KLAUSSCPU_ABS64 00000000 lf + 0
# NOTEXT-NEXT: R_KLAUSSCPU_ABS64 00000000 ldata + 0

## A shared library: the same, against its own preemptible symbols.
# RUN: not ld.lld -shared abs32.o lib.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=SHARED32 --implicit-check-not=error:
# SHARED32:      error: relocation R_KLAUSSCPU_ABS32 cannot be used against symbol 'lf'; recompile with -fPIC
# SHARED32-NEXT: >>> defined in lib.o
# SHARED32-NEXT: >>> referenced by abs32.o:(.text+0x4)
# SHARED32:      error: relocation R_KLAUSSCPU_ABS32 cannot be used against symbol 'ldata'; recompile with -fPIC
# SHARED32:      error: relocation R_KLAUSSCPU_ABS32 cannot be used against symbol 'lf'; recompile with -fPIC
# SHARED32-NEXT: >>> defined in lib.o
# SHARED32-NEXT: >>> referenced by abs32.o:(.data+0x0)
# RUN: not ld.lld -shared ro.o lib.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=SHAREDRO --implicit-check-not=error:
# SHAREDRO:      error: relocation R_KLAUSSCPU_ABS64 cannot be used against symbol 'lf'; recompile with -fPIC
# SHAREDRO-NEXT: >>> defined in lib.o
# SHAREDRO-NEXT: >>> referenced by ro.o:(.rodata+0x0)
# SHAREDRO:      error: relocation R_KLAUSSCPU_ABS64 cannot be used against symbol 'ldata'; recompile with -fPIC

## Symbols that cannot be preempted: ABS32 has no RELATIVE form, and a
## RELATIVE cannot go into a read-only section; an absolute symbol is a
## link-time constant.
# RUN: not ld.lld -pie own.o --defsym=abs=0x1234 -o /dev/null 2>&1 | FileCheck %s --check-prefix=OWN --implicit-check-not=error:
# RUN: not ld.lld -shared own.o --defsym=abs=0x1234 -o /dev/null 2>&1 | FileCheck %s --check-prefixes=OWN,OWNSO --implicit-check-not=error:
# OWN:        error: relocation R_KLAUSSCPU_ABS32 cannot be used against symbol 'hid'; recompile with -fPIC
# OWN-NEXT:   >>> defined in own.o
# OWN-NEXT:   >>> referenced by own.o:(.text+0x4)
# OWN:        error: relocation R_KLAUSSCPU_ABS32 cannot be used against symbol 'hid'; recompile with -fPIC
# OWN-NEXT:   >>> defined in own.o
# OWN-NEXT:   >>> referenced by own.o:(.data+0x8)
## In a shared library `abs` (default visibility) can be preempted.
# OWNSO:      error: relocation R_KLAUSSCPU_ABS32 cannot be used against symbol 'abs'; recompile with -fPIC
# OWN:        error: relocation R_KLAUSSCPU_ABS64 cannot be used against symbol 'hid'; recompile with -fPIC
# OWN-NEXT:   >>> defined in own.o
# OWN-NEXT:   >>> referenced by own.o:(.rodata+0x0)
## A static link takes all of them, as before.
# RUN: ld.lld -static own.o --defsym=abs=0x1234 -o /dev/null

## What a PIE may do with a library symbol: PC-relative references (a copy
## relocation, a canonical PLT entry) and a writable ABS64.
# RUN: ld.lld -pie ok.o libx.so -o ok
# RUN: llvm-readelf -r ok | FileCheck %s --check-prefix=OK
## lf's canonical PLT entry is the st_value of its .dynsym entry, ldata's
## copy that of its.
# OK:      '.rela.dyn' {{.*}} contains 3 entries:
# OK-DAG:  R_KLAUSSCPU_ABS64 {{0*}}[[LDATA:[1-9a-f][0-9a-f]+]] ldata + 0
# OK-DAG:  R_KLAUSSCPU_COPY {{0*}}[[LDATA]] ldata + 0
# OK-DAG:  R_KLAUSSCPU_ABS64 {{0*}}[[LF:[1-9a-f][0-9a-f]+]] lf + 0
# OK:      '.rela.plt' {{.*}} contains 1 entries:
# OK-NEXT: Offset
# OK-NEXT: R_KLAUSSCPU_JUMP_SLOT {{0*}}[[LF]] lf + 0

#--- lib.s
        .text
        .globl lf
        .type lf, @function
lf:     ret
        .data
        .globl ldata
        .type ldata, @object
        .size ldata, 8
ldata:  .quad 1

#--- abs32.s
        .globl _start
_start: setru r1, lf                    # R_KLAUSSCPU_ABS32
        setru r1, ldata
        ret
        .data
        .long lf

#--- ro.s
        .text
        ret
        .section .rodata,"a",@progbits
        .p2align 3
        .quad lf                        # R_KLAUSSCPU_ABS64
        .quad ldata

#--- own.s
        .globl _start, hid
        .hidden hid
_start: setru r1, hid
        ret
        .data
        .p2align 3
hid:    .quad 0
        .long hid
        .long abs
        .section .rodata,"a",@progbits
        .p2align 3
        .quad hid

#--- ok.s
        .globl _start
_start: leapc r1, ldata                 # R_KLAUSSCPU_PCREL32
        ret
        .data
        .p2align 3
        .quad lf - .                    # R_KLAUSSCPU_PC64
        .long ldata - .                 # R_KLAUSSCPU_PC32
        .p2align 3
        .quad lf                        # R_KLAUSSCPU_ABS64
        .quad ldata
