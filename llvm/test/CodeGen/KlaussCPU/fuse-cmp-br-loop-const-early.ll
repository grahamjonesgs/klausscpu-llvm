; RUN: llc -march=klausscpu -verify-machineinstrs -O1 -stop-before=post-RA-sched \
; RUN:   < %s | FileCheck %s --check-prefix=MIR
; RUN: llc -march=klausscpu -verify-machineinstrs -O1 < %s | FileCheck %s
;
; The FuseCmpBr loop constant is set up before the post-RA scheduler
; (addPreSched2), whose anti-dependency renaming may otherwise take the free
; register it needs: the loop compare is already CMPRR with the constant
; register, live into the loop, when the scheduler runs.  The pre-emit
; instance then fuses it.

define i64 @count(ptr %p) nounwind {
; MIR-LABEL: name: count
; MIR:       bb.0.entry:
; MIR:         $[[C:r[0-9]+]] = SETR 8000
; MIR:       bb.1.loop:
; MIR:         liveins: {{.*}}$[[C]]
; MIR:         CMPRR_I $[[I:r[0-9]+]], $[[C]], implicit-def $flags
; MIR-NEXT:    JMPNE %bb.1, implicit $flags
;
; CHECK-LABEL: count:
; CHECK:         setr [[C:r[0-9]+]], 8000
; CHECK:       .LBB0_1:
; CHECK:         fbr {{r[0-9]+}}, [[C]], 8, .LBB0_1
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %sum = phi i64 [ 0, %entry ], [ %sum.next, %loop ]
  %addr = getelementptr i64, ptr %p, i64 %i
  %v = load i64, ptr %addr, align 8
  %sum.next = add i64 %sum, %v
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, 1000
  br i1 %done, label %exit, label %loop

exit:
  ret i64 %sum.next
}
