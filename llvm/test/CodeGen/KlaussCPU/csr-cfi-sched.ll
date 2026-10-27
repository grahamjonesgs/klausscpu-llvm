; RUN: llc -mtriple=klausscpu -O1 -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -mtriple=klausscpu -O2 -verify-machineinstrs < %s | FileCheck %s

; CFI must not change the schedule (-g or unwind tables vs neither).  A CFI
; instruction is a scheduling boundary, so the callee-saved registers'
; .cfi_offset rules are only added after the post-RA scheduler
; (KlaussCPUCalleeSavedCFI), each directly behind its own spill.  Built in
; emitPrologue behind all spills, they used to pin the spill to ENTER, and
; the SETR that fills ENTER's bubble without them came after the spill.

declare i64 @g(i64)

; No CFI (nounwind): the post-RA scheduler issues the SETR first.
define i64 @one_csr(i64 %x) nounwind {
; CHECK-LABEL: one_csr:
; CHECK-NOT:     .cfi
; CHECK:         enter 4
; CHECK-NEXT:    setr r0, 3
; CHECK-NEXT:    stidx64 r4, r15, -8
; CHECK-NEXT:    call g
  %a = call i64 @g(i64 3)
  %b = call i64 @g(i64 %a)
  %r = add i64 %a, %b
  ret i64 %r
}

; The same function with CFI: the same instructions in the same order.
define i64 @one_csr_cfi(i64 %x) {
; CHECK-LABEL: one_csr_cfi:
; CHECK:         .cfi_startproc
; CHECK:         enter 4
; CHECK-NEXT:    .cfi_def_cfa r15, 16
; CHECK-NEXT:    .cfi_offset r15, -16
; CHECK-NEXT:    setr r0, 3
; CHECK-NEXT:    stidx64 r4, r15, -8
; CHECK-NEXT:    .cfi_offset r4, -24
; CHECK-NEXT:    call g
  %a = call i64 @g(i64 3)
  %b = call i64 @g(i64 %a)
  %r = add i64 %a, %b
  ret i64 %r
}

; Each rule follows its own spill.
define i64 @two_csr_cfi(i64 %x, i64 %y) {
; CHECK-LABEL: two_csr_cfi:
; CHECK:         enter 5
; CHECK-NEXT:    .cfi_def_cfa r15, 16
; CHECK-NEXT:    .cfi_offset r15, -16
; CHECK-NEXT:    setr r0, 5
; CHECK-NEXT:    stidx64 r4, r15, -8
; CHECK-NEXT:    .cfi_offset r4, -24
; CHECK-NEXT:    stidx64 r5, r15, -16
; CHECK-NEXT:    .cfi_offset r5, -32
; CHECK-NEXT:    call g
  %a = call i64 @g(i64 5)
  %b = call i64 @g(i64 %a)
  %c = call i64 @g(i64 %b)
  %r1 = add i64 %a, %b
  %r = add i64 %r1, %c
  ret i64 %r
}
