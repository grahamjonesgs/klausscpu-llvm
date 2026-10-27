; The post-RA list scheduler runs at -O1, the level every production build
; uses (clang -O1 is CodeGenOptLevel::Less; the generic default is -O2 only).
; It is the only pass that can fill a load-use gap by moving a later
; independent instruction up (the pre-RA MISched schedules bottom-up), so in a
; byte-copy loop the counter and pointer updates go between the load and the
; store that uses it.  With -mcpu=no-sched (NoSchedModel, PostRAScheduler=0)
; it stays off and the store follows the load directly.  LSR is off so the
; loop keeps its two pointer IVs (it would share one index between the
; register-offset load and store, leaving nothing to move).
;
; RUN: llc -march=klausscpu -verify-machineinstrs -O1 -disable-lsr < %s \
; RUN:   | FileCheck %s
; RUN: llc -march=klausscpu -verify-machineinstrs -O1 -disable-lsr \
; RUN:   -mcpu=no-sched < %s | FileCheck %s --check-prefix=NOSCHED

define void @bytecopy(ptr %d, ptr %s, i64 %n) {
; CHECK-LABEL: bytecopy:
; CHECK:       .LBB0_1:
; CHECK:         memget8 [[V:r[0-9]+]], r1
; CHECK-NEXT:    {{(incr|decr)}}
; CHECK-NEXT:    {{(incr|decr)}}
; CHECK-NEXT:    memset8 [[V]], r0
;
; NOSCHED-LABEL: bytecopy:
; NOSCHED:     .LBB0_1:
; NOSCHED:       memget8 [[V:r[0-9]+]], r1
; NOSCHED-NEXT:  memset8 [[V]], r0
entry:
  %cmp = icmp eq i64 %n, 0
  br i1 %cmp, label %exit, label %loop

loop:
  %p = phi ptr [ %p1, %loop ], [ %s, %entry ]
  %q = phi ptr [ %q1, %loop ], [ %d, %entry ]
  %k = phi i64 [ %k1, %loop ], [ %n, %entry ]
  %v = load i8, ptr %p, align 1
  store i8 %v, ptr %q, align 1
  %p1 = getelementptr inbounds i8, ptr %p, i64 1
  %q1 = getelementptr inbounds i8, ptr %q, i64 1
  %k1 = add i64 %k, -1
  %done = icmp eq i64 %k1, 0
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
