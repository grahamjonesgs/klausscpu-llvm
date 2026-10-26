; RUN: llc -march=klausscpu -mcpu=no-sched -verify-machineinstrs -O0 < %s | FileCheck %s
; RUN: llc -march=klausscpu -mcpu=no-sched -verify-machineinstrs -O1 < %s | FileCheck %s
; RUN: llc -march=klausscpu -mcpu=no-sched -verify-machineinstrs -O2 < %s | FileCheck %s

; Selects computing call arguments are scheduled inside the call sequence
; (between ADJCALLSTACKDOWN and ADJCALLSTACKUP) without a scheduling model.
; The blocks the SELECT_CC inserter creates must start with that call frame
; set up, or the machine verifier reports "Call frame size on entry does not
; match value computed from predecessor".

declare void @g(i64, i64, i64, i64, i64, i64)

; CHECK-LABEL: f:
; CHECK:         call g
define void @f(i64 %a, i64 %b, i64 %c, i64 %d) nounwind {
  %c1 = icmp slt i64 %a, %b
  %s1 = select i1 %c1, i64 %c, i64 %d
  %c2 = icmp ult i64 %c, %d
  %s2 = select i1 %c2, i64 %a, i64 %b
  call void @g(i64 %a, i64 %b, i64 %c, i64 %d, i64 %s1, i64 %s2)
  ret void
}
