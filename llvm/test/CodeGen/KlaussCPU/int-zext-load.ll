; RUN: llc -march=klausscpu -verify-machineinstrs -O1 < %s | FileCheck %s
;
; Sub-i64 loads zero-extend in hardware, so an `int` loaded for an equality
; compare needs no separate ZEXTW.  The short-circuit `a != c || ...` below is
; a `select i1 a, i1 b, false`, which SelectionDAG lowers with FREEZE on the
; compare operands; the FREEZE used to hide the load from the ZEXTLOAD fold
; and left `memget32 rX; ...; zextw rX, rX` in the N-queens inner loop.

; Reduced from the N-queens inner loop (`dc = c - cols[r]; ...
; if (cols[r] == c || dc == row - r)`).
define i32 @queens_inner(ptr %cols, i32 %c, i32 %row, i64 %n) {
; CHECK-LABEL: queens_inner:
; CHECK:         memget32 [[L:r[0-9]+]], r0
; CHECK-NOT:     zextw [[L]], [[L]]
; CHECK:         jmp
entry:
  br label %body
body:
  %iv = phi i64 [ %iv.next, %body ], [ 0, %entry ]
  %legal = phi i32 [ %sel, %body ], [ 1, %entry ]
  %p = getelementptr inbounds i32, ptr %cols, i64 %iv
  %v = load i32, ptr %p, align 4
  %sub = sub nsw i32 %c, %v
  %dc = call i32 @llvm.abs.i32(i32 %sub, i1 true)
  %ne1 = icmp ne i32 %v, %c
  %t = trunc i64 %iv to i32
  %dr = sub i32 %row, %t
  %ne2 = icmp ne i32 %dc, %dr
  %ok = select i1 %ne1, i1 %ne2, i1 false
  %sel = select i1 %ok, i32 %legal, i32 0
  %iv.next = add nuw nsw i64 %iv, 1
  %more = icmp slt i64 %iv.next, %n
  %cont = select i1 %ok, i1 %more, i1 false
  br i1 %cont, label %body, label %exit
exit:
  ret i32 %sel
}

; A loaded value used for an equality compare needs no ZEXTW either way.
define i64 @plain_load_cmp(ptr %p, i32 %c) {
; CHECK-LABEL: plain_load_cmp:
; CHECK:         memget32 [[L:r[0-9]+]], r0
; CHECK-NOT:     zextw [[L]]
; CHECK:         ret
  %l = load i32, ptr %p, align 4
  %e = icmp eq i32 %l, %c
  %r = zext i1 %e to i64
  ret i64 %r
}

declare i32 @llvm.abs.i32(i32, i1)
