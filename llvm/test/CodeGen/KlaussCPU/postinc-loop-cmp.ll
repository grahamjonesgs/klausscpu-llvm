; RUN: llc -march=klausscpu -verify-machineinstrs -O1 < %s | FileCheck %s
; RUN: llc -march=klausscpu -verify-machineinstrs -O1 -klausscpu-postinc-loop-cmp=false < %s | FileCheck %s --check-prefix=OFF
;
; KlaussCPUPostIncLoopCmp: InstCombine turns the exit test of
; `for (i = a; i < N; i += K)` into `icmp slt %i, N - K` on the counter
; BEFORE its update.  The update then sits between that compare and the
; branch, and KlaussCPUFuseCmpBr cannot fuse the pair (the compare operand is
; redefined before the branch): CMPRV + copy + JMPLT.  With the compare on
; the updated counter (the add's nsw/nuw makes it exact) the loop ends in
; ADDI + one fused branch.

@sink = external global i64

; signed, step 3: i < 997 is i + 3 < 1000
define void @signed_step(i64 %a) nounwind {
; CHECK-LABEL: signed_step:
; CHECK:       addi r0, r0, 3
; CHECK-NEXT:  fbr r0, r14, 1, .LBB0_2
; CHECK-NOT:   cmprv
; OFF-LABEL: signed_step:
; OFF:       cmprv r0, 997
entry:
  %g = icmp slt i64 %a, 1000
  br i1 %g, label %loop, label %exit

loop:
  %i = phi i64 [ %a, %entry ], [ %next, %loop ]
  store volatile i64 %i, ptr @sink
  %next = add nsw i64 %i, 3
  %c = icmp slt i64 %i, 997
  br i1 %c, label %loop, label %exit

exit:
  ret void
}

; unsigned, step 7 (nuw)
define void @unsigned_step(i64 %a) nounwind {
; CHECK-LABEL: unsigned_step:
; CHECK:       addi r0, r0, 7
; CHECK-NEXT:  fbr r0, r14, 3, .LBB1_2
; CHECK-NOT:   cmprv
entry:
  %g = icmp ult i64 %a, 5000
  br i1 %g, label %loop, label %exit

loop:
  %i = phi i64 [ %a, %entry ], [ %next, %loop ]
  store volatile i64 %i, ptr @sink
  %next = add nuw i64 %i, 7
  %c = icmp ult i64 %i, 4993
  br i1 %c, label %loop, label %exit

exit:
  ret void
}

; No wrap flag: the rewrite would not be exact, the compare stays on the old
; counter.
define void @no_flag(i64 %a) nounwind {
; CHECK-LABEL: no_flag:
; CHECK:       cmprv r0, 997
entry:
  %g = icmp slt i64 %a, 1000
  br i1 %g, label %loop, label %exit

loop:
  %i = phi i64 [ %a, %entry ], [ %next, %loop ]
  store volatile i64 %i, ptr @sink
  %next = add i64 %i, 3
  %c = icmp slt i64 %i, 997
  br i1 %c, label %loop, label %exit

exit:
  ret void
}

; Only nuw, but a signed compare: left alone.
define void @wrong_flag(i64 %a) nounwind {
; CHECK-LABEL: wrong_flag:
; CHECK:       cmprv r0, 997
entry:
  %g = icmp slt i64 %a, 1000
  br i1 %g, label %loop, label %exit

loop:
  %i = phi i64 [ %a, %entry ], [ %next, %loop ]
  store volatile i64 %i, ptr @sink
  %next = add nuw i64 %i, 3
  %c = icmp slt i64 %i, 997
  br i1 %c, label %loop, label %exit

exit:
  ret void
}

; N + K wraps (limit near INT64_MAX): left alone.
define void @limit_overflows(i64 %a) nounwind {
; CHECK-LABEL: limit_overflows:
; CHECK-NOT:   addi r0, r0, 3
; CHECK:       cmprr
entry:
  %g = icmp slt i64 %a, 1000
  br i1 %g, label %loop, label %exit

loop:
  %i = phi i64 [ %a, %entry ], [ %next, %loop ]
  store volatile i64 %i, ptr @sink
  %next = add nsw i64 %i, 3
  %c = icmp slt i64 %i, 9223372036854775806
  br i1 %c, label %loop, label %exit

exit:
  ret void
}
