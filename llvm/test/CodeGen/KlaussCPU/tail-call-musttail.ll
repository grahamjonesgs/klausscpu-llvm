; RUN: llc -mtriple=klausscpu -verify-machineinstrs -O1 < %s | FileCheck %s
; RUN: llc -mtriple=klausscpu -verify-machineinstrs -O0 < %s | FileCheck %s
; RUN: sed 's/^;BAD //' %s | not llc -mtriple=klausscpu -O1 -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --check-prefix=ERR
;
; musttail works where a tail call is possible, also at -O0, and is an error
; where it is not (here: a stack argument).

declare i64 @g(i64)
declare i64 @g5(i64, i64, i64, i64, i64)

; CHECK-LABEL: mt:
; CHECK-NOT:     enter
; CHECK:         tail g
define i64 @mt(i64 %a) nounwind {
  %r = musttail call i64 @g(i64 %a)
  ret i64 %r
}

;BAD define i64 @mt5(i64 %a, i64 %b, i64 %c, i64 %d, i64 %e) nounwind {
;BAD   %r = musttail call i64 @g5(i64 %a, i64 %b, i64 %c, i64 %d, i64 %e)
;BAD   ret i64 %r
;BAD }
; ERR: KlaussCPU: failed to perform a musttail call
