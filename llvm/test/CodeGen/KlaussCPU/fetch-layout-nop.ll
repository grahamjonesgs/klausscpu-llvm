; RUN: llc -march=klausscpu -verify-machineinstrs -O2 < %s | FileCheck %s
; RUN: llc -march=klausscpu -verify-machineinstrs -O2 -klausscpu-fetch-line-layout=1 < %s \
; RUN:   | FileCheck %s --check-prefix=MOVE
;
; The 2-word `xorv` would sit at loop offset 0xc and cross a 16-byte fetch
; line.  Every instruction in front of it is its producer or independent of
; nothing movable, so no move helps; a NOP in front of it (it fills the
; addi -> xorv stall, so it costs nothing here) starts the xorv a line.
; Level 1 (moves only) and optimising for size leave the loop alone.
;
; CHECK-LABEL: f:
; CHECK:       addi r14, r14, 10
; CHECK-NEXT:  nop
; CHECK-NEXT:  xorv r1, r14, 878082192
;
; MOVE-LABEL: f:
; MOVE:       addi r14, r14, 10
; MOVE-NEXT:  xorv r1, r14, 878082192
define i64 @f(i64 %n, i64 %a) nounwind {
entry:
  br label %l
l:
  %i = phi i64 [0, %entry], [%i.next, %l]
  %x = phi i64 [%a, %entry], [%x5, %l]
  %x1 = add i64 %x, 1
  %x2 = mul i64 %x1, 3
  %x3 = add i64 %x2, 7
  %x4 = add i64 %x3, 0
  %x5 = xor i64 %x4, 878082192
  %i.next = add i64 %i, 1
  %cc = icmp ult i64 %i.next, %n
  br i1 %cc, label %l, label %e
e:
  ret i64 %x5
}
