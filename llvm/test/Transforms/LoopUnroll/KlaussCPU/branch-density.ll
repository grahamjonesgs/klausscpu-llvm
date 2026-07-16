; M8 unroll budget is gated on the loop body's branch density (spec §7.4).
;
; The budget assumes the scheduler can interleave the unrolled copies to fill the
; 2-cycle dependent-chain gaps.  That premise only holds when the copies land in
; ONE basic block: MachineScheduler is block-local, and KlaussCPU has no CMOV, so
; an if/else body is never if-converted flat.  Copies of a branch-dense body sit
; in blocks the scheduler cannot interleave — all of the fetch cost, none of the
; DATA win (board, `branchy`: DATA 18.6%->13.1%, IF_MISS 52.2%->55.1%, net +6.5%).
;
; GATE pins the density-scaled budget; FLAT pins the ungated budget, proving the
; gate (not some other cost-model effect) is what separates these loops.
;
; RUN: opt -passes=loop-unroll -S < %s | FileCheck %s --check-prefixes=CHECK,GATE
; RUN: opt -passes=loop-unroll -klausscpu-unroll-branch-density-gate=false -S < %s \
; RUN:   | FileCheck %s --check-prefixes=CHECK,FLAT

target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64"
target triple = "klausscpu-unknown-elf"

; Straight-line body: 0 internal branches.  All copies land in one block, so the
; scheduler can interleave them — this is the case M8 was tuned for.  The gate
; must leave it at the full 4x budget.
;
; CHECK-LABEL: @straight(
; CHECK: %v.3 = load
; CHECK-NOT: %v.4 = load
define void @straight(ptr %a, i64 %n) {
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

; One if/else diamond: 1 internal branch -> budget halved to 2x.
;
; CHECK-LABEL: @branchy1(
; GATE: %v.1 = load
; GATE-NOT: %v.2 = load
; FLAT: %v.3 = load
define void @branchy1(ptr %a, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %latch ]
  %p = getelementptr i64, ptr %a, i64 %i
  %v = load i64, ptr %p
  %c1 = icmp sgt i64 %v, 0
  br i1 %c1, label %then, label %latch
then:
  %s = add i64 %v, 1
  store i64 %s, ptr %p
  br label %latch
latch:
  %i.next = add i64 %i, 1
  %c = icmp slt i64 %i.next, %n
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

; Selects are branches here.  KlaussCPU has no CMOV, so each select is lowered to
; a compare + branch + PHI.  SimplifyCFG speculates `if (c) cnt++;` into a select
; long before loop-unroll runs, so this body's *terminators* say "straight-line"
; while the emitted code is branch-dense.  This is the `branchy` kernel's exact
; shape (3 source ifs -> selects) and the case a terminator-only count misses:
; without select counting the gate reads density 0 and unrolls this 4x.
;
; CHECK-LABEL: @selects(
; GATE-NOT: %v.1 = load
; FLAT: %v.3 = load
define i64 @selects(ptr %a, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %cnt = phi i64 [ 0, %entry ], [ %cnt3, %loop ]
  %p = getelementptr i64, ptr %a, i64 %i
  %v = load i64, ptr %p
  %c1 = icmp sgt i64 %v, 0
  %a1 = add i64 %cnt, 1
  %cnt1 = select i1 %c1, i64 %a1, i64 %cnt
  %c2 = icmp slt i64 %v, 100
  %a2 = add i64 %cnt1, 2
  %cnt2 = select i1 %c2, i64 %a2, i64 %cnt1
  %c3 = icmp eq i64 %v, 42
  %a3 = add i64 %cnt2, 3
  %cnt3 = select i1 %c3, i64 %a3, i64 %cnt2
  %i.next = add i64 %i, 1
  %c = icmp slt i64 %i.next, %n
  br i1 %c, label %loop, label %exit
exit:
  ret i64 %cnt3
}

; Dense control flow: 3 internal branches -> no body duplication at all.
;
; CHECK-LABEL: @branchy3(
; GATE-NOT: %v.1 = load
; FLAT: %v.3 = load
define void @branchy3(ptr %a, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %latch ]
  %p = getelementptr i64, ptr %a, i64 %i
  %v = load i64, ptr %p
  %c1 = icmp sgt i64 %v, 0
  br i1 %c1, label %b2, label %latch
b2:
  %c2 = icmp slt i64 %v, 100
  br i1 %c2, label %b3, label %latch
b3:
  %c3 = icmp eq i64 %v, 42
  br i1 %c3, label %then, label %latch
then:
  %s = add i64 %v, 1
  store i64 %s, ptr %p
  br label %latch
latch:
  %i.next = add i64 %i, 1
  %c = icmp slt i64 %i.next, %n
  br i1 %c, label %loop, label %exit
exit:
  ret void
}
