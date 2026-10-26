# RUN: llvm-mc -triple=klausscpu -filetype=obj %s -o %t.o
# RUN: llvm-objdump -s -j .data %t.o | FileCheck %s
# RUN: llvm-readobj -r %t.o | FileCheck %s --check-prefix=RELOC

# Data fixups of every size.  1- and 2-byte label differences are written
# (they used to be dropped as 0).  A cross-section difference is PC-relative:
# 4 bytes -> R_KLAUSSCPU_PC32, 8 bytes -> R_KLAUSSCPU_PC64 (it used to be
# R_KLAUSSCPU_ABS64, losing the "- B").  With RELA the addend is in r_addend
# only; the section contents of a relocated field are 0 (they used to hold
# the addend as well).

        .text
func:
        nop
        nop
.Lend:

        .data
dat:
        .byte   .Lend - func            # 0x00: 08
        .short  .Lend - func            # 0x01: 0008
        .byte   end - dat               # 0x03: 33
        .short  end - dat               # 0x04: 0033
        .long   ext - .                 # 0x06: PC32 ext
        .quad   ext - .                 # 0x0a: PC64 ext
        .quad   ext + 8                 # 0x12: ABS64 ext+8
        .long   ext + 3                 # 0x1a: ABS32 ext+3
        .quad   func - .                # 0x1e: PC64 .text+0
        .long   .Lend - .               # 0x26: PC32 .text+8
        .quad   .Lend - func            # 0x2a: 8 (resolved)
        .byte   dat - end               # 0x32: cd (-0x33)
end:

# CHECK:      Contents of section .data:
# CHECK-NEXT:  0000 08080033 33000000 00000000 00000000
# CHECK-NEXT:  0010 00000000 00000000 00000000 00000000
# CHECK-NEXT:  0020 00000000 00000000 00000800 00000000
# CHECK-NEXT:  0030 0000cd

# RELOC:      Section ({{[0-9]+}}) .rela.data {
# RELOC-NEXT:   0x6 R_KLAUSSCPU_PC32 ext 0x0
# RELOC-NEXT:   0xA R_KLAUSSCPU_PC64 ext 0x0
# RELOC-NEXT:   0x12 R_KLAUSSCPU_ABS64 ext 0x8
# RELOC-NEXT:   0x1A R_KLAUSSCPU_ABS32 ext 0x3
# RELOC-NEXT:   0x1E R_KLAUSSCPU_PC64 .text 0x0
# RELOC-NEXT:   0x26 R_KLAUSSCPU_PC32 .text 0x8
# RELOC-NEXT: }
