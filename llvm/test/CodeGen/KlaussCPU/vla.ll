; RUN: llc -march=klausscpu -verify-machineinstrs -O2 < %s | FileCheck %s
;
; Dynamic allocas (VLAs).  GETSP_R must be re-read after every SETSP_R/ADDSP:
; an unchained GETSP_R has no operands, so SelectionDAG used to CSE every one
; in the block into a single node — two VLAs were carved from the same SP and
; overlapped, and stack arguments after a VLA were stored relative to the
; pre-VLA SP (above the callee's incoming arguments).

declare void @use(ptr, ptr)
declare void @many(i64, i64, i64, i64, i64, i64)

; Second VLA is based on the SP left by the first one.
define void @two_vlas(i64 %n, i64 %m) {
; CHECK-LABEL: two_vlas:
; CHECK:         getsp r15
; CHECK:         getsp [[SP1:r[0-9]+]]
; CHECK:         subr [[A:r[0-9]+]], [[SP1]], {{r[0-9]+}}
; CHECK-NEXT:    setsp [[A]]
; CHECK:         getsp [[SP2:r[0-9]+]]
; CHECK-NEXT:    subr [[B:r[0-9]+]], [[SP2]], {{r[0-9]+}}
; CHECK-NEXT:    setsp [[B]]
; CHECK:         call use
  %a = alloca i8, i64 %n, align 8
  %b = alloca i8, i64 %m, align 8
  call void @use(ptr %a, ptr %b)
  ret void
}

; No reserved call frame (VLA present): stack args are stored relative to the
; SP read *after* ADJCALLSTACKDOWN's addsp.
define void @vla_stackargs(i64 %n) {
; CHECK-LABEL: vla_stackargs:
; CHECK:         call use
; CHECK:         addsp -40
; CHECK-NEXT:    getsp [[SP:r[0-9]+]]
; CHECK:         stidx64 {{r[0-9]+}}, [[SP]], 32
; CHECK-NEXT:    stidx64 {{r[0-9]+}}, [[SP]], 24
; CHECK-NEXT:    call many
; CHECK-NEXT:    addsp 40
  %a = alloca i8, i64 %n, align 8
  call void @use(ptr %a, ptr %a)
  call void @many(i64 1, i64 2, i64 3, i64 4, i64 5, i64 6)
  ret void
}

; A call without stack arguments does not read SP at all.
define void @no_stackargs(ptr %p) {
; CHECK-LABEL: no_stackargs:
; CHECK:         getsp r15
; CHECK-NOT:     getsp
; CHECK:         call use
  call void @use(ptr %p, ptr %p)
  ret void
}
