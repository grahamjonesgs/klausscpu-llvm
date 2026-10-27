; A taken FBR squashes the instruction after it in EX, and a divide there has
; already started the RTL divider, which the next real divide then reads
; (KlaussCPUFuseCmpBr.cpp). So a compare and branch whose fall-through starts
; with a divide stays CMPRV + JMPcc; one whose fall-through starts with
; anything else still fuses.
;
; RUN: llc -mtriple=klausscpu -O1 -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -mtriple=klausscpu -O2 -verify-machineinstrs < %s | FileCheck %s

; if (b == 0 || b == -1) return 0; return a / b == 0 ? 1 : 2;
; The guard falls through to the divide; the compare of the quotient does not.
define i32 @guarded_div(i64 %a, i64 %b) {
; CHECK-LABEL: guarded_div:
; CHECK:       cmprv r{{[0-9]+}}, 2
; CHECK-NEXT:  jmpultrel .LBB0_[[#]]
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
; exit block starts with the divide: each taken FBR would start a wrong-path
; divide, and the real one at the exit would return the previous iteration's
; quotient.
define i64 @loop_exit_div(ptr %p, i64 %n, i64 %d) {
; CHECK-LABEL: loop_exit_div:
; CHECK:       .LBB1_1:
; CHECK:       cmprv r1, 0
; CHECK-NEXT:  jmpnerel .LBB1_1
; CHECK-NEXT:  # %bb.2:
; CHECK-NEXT:  divur r12, r12, r2
; CHECK-NOT:   fbr
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
; CHECK:       cmprv r1, 0
; CHECK-NEXT:  jmpnerel .LBB2_1
; CHECK-NEXT:  # %bb.2:
; CHECK-NEXT:  modr r12, r12, r2
; CHECK-NOT:   fbr
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
