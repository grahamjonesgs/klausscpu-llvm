; RUN: llc -march=klausscpu -relocation-model=pic -filetype=obj < %s \
; RUN:   | llvm-readelf -r - | FileCheck %s
;
; A cross-section symbol difference (A - B) in read-only data — the exact shape
; the -fPIC relative-lookup-table optimization (RelLookupTableConverter) emits:
;   .long <str> - <table>   loaded via llvm.load.relative.
; It must lower to a *data* PC-relative relocation, R_KLAUSSCPU_PC32 (type 4),
; NOT R_KLAUSSCPU_ABS32 (type 1).  Emitting ABS32 silently drops the "- B" term
; and miscompiles the table into absolute addresses (see the AES-CTR vector
; corruption that motivated this).  The relocation renders as "Unknown" because
; the dumper has no name table for this experimental target; the Info field's
; low byte is the type (04 = PC32, 01 = the old ABS32 bug).

@s = private unnamed_addr constant [2 x i8] c"a\00", section ".rodata.str1.1"
@t = private unnamed_addr constant [1 x i32]
       [i32 trunc (i64 sub (i64 ptrtoint (ptr @s to i64),
                            i64 ptrtoint (ptr @t to i64)) to i32)]

; CHECK: .rela.rodata
; type byte 04 = R_KLAUSSCPU_PC32 (would be 01 for the ABS32 miscompile)
; CHECK: 00000000 00000204 {{.*}} .rodata.str1.1
