; RUN: llc -march=klausscpu -O1 -verify-machineinstrs -filetype=null \
; RUN:   -print-after=klausscpu-iv-inc < %s 2>&1 | FileCheck %s --check-prefix=SCHED
; RUN: llc -march=klausscpu -mcpu=no-sched -O1 -verify-machineinstrs \
; RUN:   -filetype=null -print-after=klausscpu-iv-inc < %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=NOSCHED
;
; KlaussCPUEarlyIVInc exists for the schedulers, so -mcpu=no-sched (the A/B
; baseline without them, NoSchedModel) leaves the loop counter alone.

; SCHED:        %i.prev = phi i64 [ -1, %entry ], [ %i1, %loop ]
; SCHED:        %done = icmp eq i64 %i1, 99

; NOSCHED:      %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
; NOSCHED:      %i.next = add nuw nsw i64 %i, 1
; NOSCHED-NEXT: %done = icmp eq i64 %i.next, 100

define i64 @data_use() nounwind {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %b = phi i64 [ 1, %entry ], [ %b.next, %loop ]
  %s = shl i64 %b, 3
  %x = xor i64 %s, %b
  %b.next = add i64 %x, %i
  %i.next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %i.next, 100
  br i1 %done, label %exit, label %loop

exit:
  ret i64 %b.next
}
