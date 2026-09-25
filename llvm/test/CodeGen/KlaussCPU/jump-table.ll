; RUN: llc -march=klausscpu -verify-machineinstrs -O2 \
; RUN:   -klausscpu-min-jump-table-entries=4 < %s | FileCheck %s
; RUN: llc -march=klausscpu -verify-machineinstrs -O2 -relocation-model=pic \
; RUN:   -klausscpu-min-jump-table-entries=4 < %s | FileCheck %s --check-prefix=PIC
; RUN: llc -march=klausscpu -verify-machineinstrs -O2 < %s \
; RUN:   | FileCheck %s --check-prefix=OFF
;
; Jump tables use EK_Custom32: each entry is the ABSOLUTE block address, in PIC
; mode too.  The generic BR_JT expansion treats PIC tables as relative and
; added the table base to the loaded entry (jumping to table+target), which
; broke every jump table in the -fPIC baremetal builds.  LowerBR_JT loads
; the entry (zero-extended) and jumps to it directly.  Jump tables are still
; off by default (OFF) pending an on-board re-test.

define i64 @sw(i64 %x) {
; CHECK-LABEL: sw:
; CHECK:         setr [[BASE:r[0-9]+]], .LJTI0_0
; CHECK:         addr [[E:r[0-9]+]], [[BASE]], {{r[0-9]+}}
; CHECK-NEXT:    memget32 [[T:r[0-9]+]], [[E]]
; CHECK-NEXT:    jmpr [[T]]
; CHECK:       .LJTI0_0:
; CHECK-NEXT:    .long .LBB0_
;
; PIC-LABEL: sw:
; PIC:           leapc [[BASE:r[0-9]+]], .LJTI0_0
; PIC:           addr [[E:r[0-9]+]], [[BASE]], {{r[0-9]+}}
; PIC-NEXT:      memget32 [[T:r[0-9]+]], [[E]]
; PIC-NEXT:      jmpr [[T]]
; PIC:         .LJTI0_0:
; PIC-NEXT:      .long .LBB0_
;
; OFF-LABEL: sw:
; OFF-NOT:       jmpr
; OFF-NOT:       .LJTI
entry:
  switch i64 %x, label %d [ i64 0, label %a  i64 1, label %b  i64 2, label %c
                            i64 3, label %e  i64 4, label %f ]
a: ret i64 10
b: ret i64 20
c: ret i64 30
e: ret i64 40
f: ret i64 50
d: ret i64 0
}
