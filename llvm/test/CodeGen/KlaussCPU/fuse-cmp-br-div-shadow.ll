; A taken FBR squashes the instruction after it in EX (its "shadow").  Since
; klaussCPU 9ad6614, the oldest core the backend supports, that squash also
; resets the divider and gates the LCD strobe, so a divide or an LCD write in
; the shadow has no effect.  A compare and branch whose fall-through starts
; with one fuses like any other (before, it stayed CMPRV + JMPcc).
;
; RUN: llc -mtriple=klausscpu -O1 -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -mtriple=klausscpu -O2 -verify-machineinstrs < %s | FileCheck %s
; The assembler does not warn about the shape either (MC/KlaussCPU/
; fbr-shadow-warning.s), also in inline asm (@asm_shadow).
; RUN: llc -mtriple=klausscpu -O2 -verify-machineinstrs -filetype=obj \
; RUN:   -o /dev/null < %s 2>&1 | count 0

; if (b == 0 || b == -1) return 0; return a / b == 0 ? 1 : 2;
; The guard falls through to the divide; the compare of the quotient does not.
define i32 @guarded_div(i64 %a, i64 %b) {
; CHECK-LABEL: guarded_div:
; CHECK:       fbri r{{[0-9]+}}, 2, 3, .LBB0_[[#]]
; CHECK-NEXT:  # %bb.1:
; CHECK-NEXT:  divr [[Q:r[0-9]+]], r0, r1
; CHECK:       fbri [[Q]], 0, 0, .LBB0_[[#]]
entry:
  %0 = add i64 %b, 1
  %or.cond = icmp ult i64 %0, 2
  br i1 %or.cond, label %return, label %if.end

if.end:
  %div = sdiv i64 %a, %b
  %cmp2 = icmp eq i64 %div, 0
  %. = select i1 %cmp2, i32 1, i32 2
  br label %return

return:
  %retval.1 = phi i32 [ %., %if.end ], [ 0, %entry ]
  ret i32 %retval.1
}

; The loop's latch branch is taken on every iteration but the last, and its
; exit block starts with the divide: each taken FBR squashes a wrong-path
; divide, which on cores before 9ad6614 kept the divider running and handed
; the real one at the exit the previous iteration's quotient.
define i64 @loop_exit_div(ptr %p, i64 %n, i64 %d) {
; CHECK-LABEL: loop_exit_div:
; CHECK:       .LBB1_1:
; CHECK:       fbri r1, 0, 8, .LBB1_1
; CHECK-NEXT:  # %bb.2:
; CHECK-NEXT:  divur r12, r12, r2
entry:
  br label %do.body

do.body:
  %s.0 = phi i64 [ 0, %entry ], [ %add, %do.body ]
  %i.0 = phi i64 [ 0, %entry ], [ %inc, %do.body ]
  %arrayidx = getelementptr inbounds nuw [8 x i8], ptr %p, i64 %i.0
  %0 = load i64, ptr %arrayidx, align 8
  %add = add i64 %0, %s.0
  %inc = add i64 %i.0, 1
  %cmp.not = icmp eq i64 %inc, %n
  br i1 %cmp.not, label %do.end, label %do.body

do.end:
  %div = udiv i64 %add, %d
  ret i64 %div
}

; The same with a signed remainder at the exit.
define i64 @loop_exit_rem(ptr %p, i64 %n, i64 %d) {
; CHECK-LABEL: loop_exit_rem:
; CHECK:       .LBB2_1:
; CHECK:       fbri r1, 0, 8, .LBB2_1
; CHECK-NEXT:  # %bb.2:
; CHECK-NEXT:  modr r12, r12, r2
entry:
  br label %do.body

do.body:
  %s.0 = phi i64 [ 0, %entry ], [ %add, %do.body ]
  %i.0 = phi i64 [ 0, %entry ], [ %inc, %do.body ]
  %arrayidx = getelementptr inbounds nuw [8 x i8], ptr %p, i64 %i.0
  %0 = load i64, ptr %arrayidx, align 8
  %add = add i64 %0, %s.0
  %inc = add i64 %i.0, 1
  %cmp.not = icmp eq i64 %inc, %n
  br i1 %cmp.not, label %do.end, label %do.body

do.end:
  %rem = srem i64 %add, %d
  ret i64 %rem
}

; An LCD write (only from inline asm) at the start of a loop's exit block.
; Only a backward branch fuses here: a forward one would span the inline asm,
; whose size is unbounded.
define void @lcd_after_loop(i64 %n) {
; CHECK-LABEL: lcd_after_loop:
; CHECK:       .LBB3_1:
; CHECK:       fbr r0, r{{[0-9]+}}, 8, .LBB3_1
; CHECK-NEXT:  # %bb.2:
; CHECK-NEXT:  #APP
; CHECK-NEXT:  lcddatav 65
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %inc, %loop ]
  tail call void asm sideeffect "", "r"(i64 %i)
  %inc = add i64 %i, 1
  %done = icmp eq i64 %inc, %n
  br i1 %done, label %exit, label %loop

exit:
  tail call void asm sideeffect "lcddatav 65", ""()
  ret void
}

; Hand-written fused branches with a divide and an LCD write behind them.
define void @asm_shadow() nounwind {
; CHECK-LABEL: asm_shadow:
; CHECK:       fbr r0, r1, 0, [[L:.Ltmp[0-9]+]]
; CHECK-EMPTY:
; CHECK-NEXT:  divr r3, r4, r5
; CHECK-EMPTY:
; CHECK-NEXT:  fbri r0, 3, 1, [[L]]
; CHECK-EMPTY:
; CHECK-NEXT:  lcddatav 65
  call void asm sideeffect "fbr r0, r1, 0, 1f\0Adivr r3, r4, r5\0Afbri r0, 3, 1, 1f\0Alcddatav 65\0A1:", "~{r3}"()
  ret void
}
