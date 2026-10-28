; RUN: llc -mtriple=klausscpu -verify-machineinstrs -O1 < %s | FileCheck %s
; RUN: llc -mtriple=klausscpu -verify-machineinstrs -O0 < %s | FileCheck %s
; RUN: sed 's/^;BAD //' %s | not llc -mtriple=klausscpu -O1 -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --check-prefix=ERR --implicit-check-not=error: \
; RUN:     --implicit-check-not="LLVM ERROR" --implicit-check-not=PLEASE
; RUN: not llc -mtriple=klausscpu -O1 -klausscpu-tail-calls=false -o /dev/null \
; RUN:   %s 2>&1 | FileCheck %s --check-prefix=OFF
;
; musttail works where a tail call is possible, also at -O0, and is an error
; where it is not (a stack argument, a variadic callee, tail calls turned
; off): a diagnostic per call, not a crash, so every one is reported.

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
;BAD declare i64 @gv(i64, ...)
;BAD define i64 @mtv(i64 %a, ...) nounwind {
;BAD   %r = musttail call i64 (i64, ...) @gv(i64 %a, ...)
;BAD   ret i64 %r
;BAD }
; ERR: error: <unknown>:0:0: in function mt5 i64 (i64, i64, i64, i64, i64): musttail call cannot be a tail call on KlaussCPU: it has stack, byval or sret arguments, is variadic, or tail calls are disabled
; ERR: error: <unknown>:0:0: in function mtv i64 (i64, ...): musttail call cannot be a tail call on KlaussCPU

; OFF: error: <unknown>:0:0: in function mt i64 (i64): musttail call cannot be a tail call on KlaussCPU
