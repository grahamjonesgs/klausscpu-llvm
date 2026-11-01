; RUN: llc -march=klausscpu -verify-machineinstrs -O2 < %s | FileCheck %s
; RUN: llc -march=klausscpu -verify-machineinstrs -O2 -klausscpu-fall-through-align-max-trips=0 < %s | FileCheck %s --check-prefix=OFF
; RUN: llc -march=klausscpu -verify-machineinstrs -O2 -klausscpu-fall-through-align-max-trips=2 < %s | FileCheck %s --check-prefix=TWO
;
; Loop-head alignment pads the fall-through path too: when the preheader
; falls into the aligned head the NOPs run once per entry.  A loop that runs
; about once per entry (a constant trip count of 1, found by
; KlaussCPULoopTripCount; or a loop that still calls its own function after
; tail-call elimination: fib) is left unaligned when it is entered by
; falling into its head.  A longer loop keeps the alignment: at two
; iterations per entry the NOPs (1.5 cycles on average) and the average
; unaligned head (1 cycle per iteration) cost the same.  The threshold is
; -klausscpu-fall-through-align-max-trips (default 1).

@a = global [64 x i64] zeroinitializer
@sink = external global i64

; A loop that runs once per entry, at the end of the entry block: no
; alignment.
define void @one_trip() nounwind {
; CHECK-LABEL: one_trip:
; CHECK-NOT:   .p2align
; CHECK:       .LBB0_1: # %loop
; OFF-LABEL: one_trip:
; OFF:         .p2align 4
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %p = getelementptr i64, ptr @a, i64 %i
  store volatile i64 %i, ptr %p
  %i.next = add nuw nsw i64 %i, 1
  %c = icmp ne i64 %i.next, 1
  br i1 %c, label %loop, label %exit

exit:
  ret void
}

; Two iterations per entry: the break-even point, aligned by default.
define void @two_trips() nounwind {
; CHECK-LABEL: two_trips:
; CHECK:       .p2align 4
; TWO-LABEL: two_trips:
; TWO-NOT:     .p2align
; TWO:         .LBB1_1: # %loop
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %p = getelementptr i64, ptr @a, i64 %i
  store volatile i64 %i, ptr %p
  %i.next = add nuw nsw i64 %i, 1
  %c = icmp ne i64 %i.next, 2
  br i1 %c, label %loop, label %exit

exit:
  ret void
}

; 64 iterations: aligned.
define void @many_trips() nounwind {
; CHECK-LABEL: many_trips:
; CHECK:       .p2align 4
; CHECK-NEXT:  .LBB2_1: # %loop
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %p = getelementptr i64, ptr @a, i64 %i
  store volatile i64 %i, ptr %p
  %i.next = add nuw nsw i64 %i, 1
  %c = icmp ne i64 %i.next, 64
  br i1 %c, label %loop, label %exit

exit:
  ret void
}

; Unknown trip count: aligned.
define void @unknown_trips(i64 %n) nounwind {
; CHECK-LABEL: unknown_trips:
; CHECK:       .p2align 4
entry:
  %g = icmp sgt i64 %n, 0
  br i1 %g, label %loop, label %exit

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %p = getelementptr i64, ptr @a, i64 %i
  store volatile i64 %i, ptr %p
  %i.next = add nuw nsw i64 %i, 1
  %c = icmp ne i64 %i.next, %n
  br i1 %c, label %loop, label %exit

exit:
  ret void
}

; Naive recursion: tail-call elimination makes a loop of one of the calls,
; and the other stays in its body.  Entered by fall-through: no alignment.
define i64 @fib(i32 %n) nounwind {
; CHECK-LABEL: fib:
; CHECK-NOT:   .p2align
; CHECK:       callrel fib
; OFF-LABEL: fib:
; OFF:         .p2align 4
entry:
  br label %tailrecurse

tailrecurse:
  %acc = phi i64 [ 0, %entry ], [ %add, %if.end ]
  %n.tr = phi i32 [ %n, %entry ], [ %sub1, %if.end ]
  %cmp = icmp ult i32 %n.tr, 2
  br i1 %cmp, label %if.then, label %if.end

if.then:
  %conv = zext nneg i32 %n.tr to i64
  %r = add i64 %acc, %conv
  ret i64 %r

if.end:
  %sub = add i32 %n.tr, -1
  %call = call i64 @fib(i32 signext %sub)
  %sub1 = add i32 %n.tr, -2
  %add = add i64 %acc, %call
  br label %tailrecurse
}
