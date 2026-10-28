; RUN: llc -mtriple=klausscpu -verify-machineinstrs -O1 < %s | FileCheck %s
; RUN: llc -mtriple=klausscpu -verify-machineinstrs -O0 < %s | FileCheck %s
; RUN: sed 's/^;BAD //' %s | not llc -mtriple=klausscpu -O1 -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --check-prefix=ERR --implicit-check-not=error: \
; RUN:     --implicit-check-not="LLVM ERROR" --implicit-check-not=PLEASE
; RUN: not llc -mtriple=klausscpu -O1 -klausscpu-tail-calls=false -o /dev/null \
; RUN:   %s 2>&1 | FileCheck %s --check-prefix=OFF
;
; musttail works where a tail call is possible, also at -O0, and is an error
; where it is not (a stack argument, a variadic callee, the address of the
; caller's own stack object - clang's copy of an aggregate over 16 bytes,
; which LEAVE frees before the callee reads it - tail calls turned off): a
; diagnostic per call, not a crash, so every one is reported.  Forwarding an
; incoming indirect aggregate (its copy is in the caller's caller) is fine.

declare i64 @g(i64)
declare i64 @g5(i64, i64, i64, i64, i64)

; CHECK-LABEL: mt:
; CHECK-NOT:     enter
; CHECK:         tail g
define i64 @mt(i64 %a) nounwind {
  %r = musttail call i64 @g(i64 %a)
  ret i64 %r
}

declare i64 @gp(ptr)

; CHECK-LABEL: mtp:
; CHECK-NOT:     enter
; CHECK:         tail gp
define i64 @mtp(ptr %s) nounwind {
  %r = musttail call i64 @gp(ptr %s)
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
;BAD define i64 @mtl(ptr %s) nounwind {
;BAD   %t = alloca [6 x i64], align 8
;BAD   call void @llvm.memcpy.p0.p0.i64(ptr align 8 %t, ptr align 8 %s, i64 48, i1 false)
;BAD   %r = musttail call i64 @gp(ptr %t)
;BAD   ret i64 %r
;BAD }
;BAD define i64 @mtg(ptr %s) nounwind {
;BAD   %t = alloca [2 x i64], align 8
;BAD   %e = getelementptr inbounds i64, ptr %t, i64 1
;BAD   store ptr %s, ptr %e
;BAD   %r = musttail call i64 @gp(ptr %e)
;BAD   ret i64 %r
;BAD }
; ERR: error: <unknown>:0:0: in function mt5 i64 (i64, i64, i64, i64, i64): musttail call cannot be a tail call on KlaussCPU: it has stack, byval or sret arguments, passes the address of a local (such as an aggregate over 16 bytes), is variadic, or tail calls are disabled
; ERR: error: <unknown>:0:0: in function mtv i64 (i64, ...): musttail call cannot be a tail call on KlaussCPU
; ERR: error: <unknown>:0:0: in function mtl i64 (ptr): musttail call cannot be a tail call on KlaussCPU
; ERR: error: <unknown>:0:0: in function mtg i64 (ptr): musttail call cannot be a tail call on KlaussCPU

; OFF: error: <unknown>:0:0: in function mt i64 (i64): musttail call cannot be a tail call on KlaussCPU
; OFF: error: <unknown>:0:0: in function mtp i64 (ptr): musttail call cannot be a tail call on KlaussCPU
