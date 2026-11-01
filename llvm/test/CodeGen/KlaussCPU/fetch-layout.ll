; RUN: llc -march=klausscpu -verify-machineinstrs -O2 < %s | FileCheck %s
; RUN: llc -march=klausscpu -verify-machineinstrs -O2 -klausscpu-fetch-line-layout=0 < %s \
; RUN:   | FileCheck %s --check-prefix=OFF
; RUN: llc -march=klausscpu -verify-machineinstrs -O2 -filetype=obj < %s \
; RUN:   | llvm-objdump -d - | FileCheck %s --check-prefix=OBJ
;
; The 3508f87 core fetches 16-byte lines: a 2-word instruction that crosses
; one takes a cycle more (KlaussCPUFetchLayout.cpp).  The loop head is
; aligned to 16, so the offsets inside the loop are known.  Before the pass the
; third `xorv` (2 words) sits at loop offset 0x1c and crosses a line; the pass
; swaps it with the independent 1-word `addi`/`incr` in front of it.
;
; OFF: xorv
; OFF: xorv
; OFF-NEXT: addi
; OFF-NEXT: xorv
;
; CHECK:       xorv r13, r2, 305419896
; CHECK-NEXT:  addi r11, r3, 3
; CHECK-NEXT:  incr r14, r1
; CHECK-NEXT:  addi r13, r13, 5
; CHECK-NEXT:  xorv r10, r14, 878082192
; CHECK-NEXT:  incr r12, r12
; CHECK-NEXT:  xorv r11, r11, 1234567
;
; With the loop head at 0x10 no 2-word instruction crosses a 16-byte line:
; OBJ:      10: {{.*}}xorv{{.*}}305419896
; OBJ:      24: {{.*}}xorv{{.*}}878082192
; OBJ:      30: {{.*}}xorv{{.*}}1234567
define i64 @f(i64 %n, i64 %a, i64 %b, i64 %c) nounwind {
entry:
  br label %l
l:
  %i = phi i64 [0, %entry], [%i.next, %l]
  %x = phi i64 [%a, %entry], [%x.next, %l]
  %y = phi i64 [%b, %entry], [%y.next, %l]
  %z = phi i64 [%c, %entry], [%z.next, %l]
  %x1 = add i64 %x, 1
  %y1 = xor i64 %y, 305419896
  %z1 = add i64 %z, 3
  %x2 = xor i64 %x1, 878082192
  %y2 = add i64 %y1, 5
  %z2 = xor i64 %z1, 1234567
  %x.next = add i64 %x2, %y2
  %y.next = add i64 %y2, %z2
  %z.next = add i64 %z2, %x1
  %i.next = add i64 %i, 1
  %cc = icmp ult i64 %i.next, %n
  br i1 %cc, label %l, label %e
e:
  %r = add i64 %x.next, %y.next
  %r2 = add i64 %r, %z.next
  ret i64 %r2
}
