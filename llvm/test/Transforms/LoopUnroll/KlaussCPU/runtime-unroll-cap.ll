; A loop without a constant trip count is unrolled at most twice
; (-klausscpu-runtime-unroll-max-count, default 2): its runtime unrolling adds
; a remainder loop and a trip-count check that a loop of few iterations per
; entry pays every time.  A loop with a constant trip count keeps the general
; cap of 4.
;
; RUN: opt -passes=loop-unroll -S < %s | FileCheck %s --check-prefixes=CHECK,CAP
; RUN: opt -passes=loop-unroll -klausscpu-runtime-unroll-max-count=0 -S < %s \
; RUN:   | FileCheck %s --check-prefixes=CHECK,NOCAP

target datalayout = "e-m:e-p:64:64-i64:64-n32:64"
target triple = "klausscpu-unknown-elf"

; CHECK-LABEL: @runtime_trips(
; CHECK: %v.1 = load
; CAP-NOT: %v.2 = load
; NOCAP: %v.3 = load
; NOCAP-NOT: %v.4 = load
define void @runtime_trips(ptr %a, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %p = getelementptr i64, ptr %a, i64 %i
  %v = load i64, ptr %p
  %s = add i64 %v, 1
  store i64 %s, ptr %p
  %i.next = add i64 %i, 1
  %c = icmp slt i64 %i.next, %n
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

; CHECK-LABEL: @constant_trips(
; CHECK: %v.3 = load
; CHECK-NOT: %v.4 = load
define void @constant_trips(ptr %a) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %p = getelementptr i64, ptr %a, i64 %i
  %v = load i64, ptr %p
  %s = add i64 %v, 1
  store i64 %s, ptr %p
  %i.next = add i64 %i, 1
  %c = icmp slt i64 %i.next, 1000
  br i1 %c, label %loop, label %exit
exit:
  ret void
}
