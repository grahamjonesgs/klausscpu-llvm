; Loop-header alignment to the 16-byte I-cache line (setPrefLoopAlignment).
;
; The core is fetch-bound and fetch works in aligned 16-byte lines, so a hot
; loop's back-edge target is aligned to a line boundary: each iteration's first
; fetch then delivers a full line of instructions instead of a partial one.
; Padding is emitted as v2 NOP (0x6C000000) by KlaussCPUAsmBackend::writeNopData
; and sits in the fall-through path, so it must be real NOPs, not zero-fill.
;
; The alignment is on by default (16 B) and controlled by -klausscpu-pref-loop-
; align for A/B (0 = off), so both states are pinned here.
;
; RUN: llc -march=klausscpu -O2 < %s | FileCheck %s --check-prefixes=CHECK,ALIGN
; RUN: llc -march=klausscpu -O2 -klausscpu-pref-loop-align=0 < %s \
; RUN:   | FileCheck %s --check-prefixes=CHECK,NOALIGN
; RUN: llc -march=klausscpu -O2 -klausscpu-pref-loop-align=32 < %s \
; RUN:   | FileCheck %s --check-prefixes=CHECK,ALIGN32

; A simple counted reduction loop.  The header block gets aligned; the NOALIGN
; run proves the directive comes from the hook, not from something else.
;
; CHECK-LABEL: reduce:
; ALIGN:        .p2align 4
; ALIGN32:      .p2align 5
; CHECK:      .LBB0_{{[0-9]+}}: # %loop
; NOALIGN-NOT:  .p2align 4
define i64 @reduce(ptr %a, i64 %n) {
entry:
  %z = icmp sgt i64 %n, 0
  br i1 %z, label %loop, label %exit
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i64 [ 0, %entry ], [ %acc.next, %loop ]
  %p = getelementptr i64, ptr %a, i64 %i
  %v = load i64, ptr %p
  %acc.next = add i64 %acc, %v
  %i.next = add i64 %i, 1
  %c = icmp slt i64 %i.next, %n
  br i1 %c, label %loop, label %exit
exit:
  %r = phi i64 [ 0, %entry ], [ %acc.next, %loop ]
  ret i64 %r
}
