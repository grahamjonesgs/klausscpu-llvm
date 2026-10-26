; The post-RA list scheduler runs at -O1, the level every production build
; uses (clang -O1 is CodeGenOptLevel::Less; the generic default is -O2 only).
; It is the only pass that can fill a load-use gap by moving a later
; independent instruction up (the pre-RA MISched schedules bottom-up), so in a
; byte-copy loop the counter and pointer updates go between the load and the
; store that uses it.  With -mcpu=no-sched (NoSchedModel, PostRAScheduler=0)
; it stays off and the store follows the load directly.
;
; RUN: llc -march=klausscpu -verify-machineinstrs -O1 < %s | FileCheck %s
; RUN: llc -march=klausscpu -verify-machineinstrs -O1 -mcpu=no-sched < %s \
; RUN:   | FileCheck %s --check-prefix=NOSCHED

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
  %i = phi i64 [ %inc, %loop ], [ 0, %entry ]
  %src = getelementptr inbounds i8, ptr %s, i64 %i
  %v = load i8, ptr %src, align 1
  %dst = getelementptr inbounds i8, ptr %d, i64 %i
  store i8 %v, ptr %dst, align 1
  %inc = add nuw i64 %i, 1
  %done = icmp eq i64 %inc, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
