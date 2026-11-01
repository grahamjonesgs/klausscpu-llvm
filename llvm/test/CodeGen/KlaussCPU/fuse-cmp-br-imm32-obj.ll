; RUN: llc -march=klausscpu -O1 -verify-machineinstrs -klausscpu-fuse-loop-const=false \
; RUN:   -filetype=obj < %s | llvm-objdump -d - | FileCheck %s
;
; The 2-word FBR_RV (rhs = sext(imm32) in word 1) counts its simm13 word
; displacement from its own first word, forward and backward, and the
; instruction after it starts 8 bytes on (ISA_ENCODING_V2_MAP.md §8.1).
; The 64-bit compares here have constants outside simm4; without a loop
; constant both become FBR_RV.

; CHECK-LABEL: <f>:
; CHECK-NEXT:     0: 10 09 20 b4 a0 86 01 00 fbrv r1, 100000, 0, 0x24 <f+0x24>
; CHECK-NEXT:     8: {{.*}} setr r12, 0
; CHECK:         10: {{.*}} stidx64 r12, r0, 0
; CHECK:         1c: c0 fd bf b4 70 11 01 00 fbrv r12, 70000, 1, 0x10 <f+0x10>
; CHECK-NEXT:    24: {{.*}} ret
define void @f(ptr %p, i64 %n) nounwind {
entry:
  %c0 = icmp eq i64 %n, 100000
  br i1 %c0, label %exit, label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %a = getelementptr i64, ptr %p, i64 %i
  store volatile i64 %i, ptr %a
  %i.next = add i64 %i, 1
  %c = icmp slt i64 %i.next, 70000
  br i1 %c, label %loop, label %exit
exit:
  ret void
}
