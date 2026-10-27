; RUN: llc -march=klausscpu -verify-machineinstrs -O1 < %s \
; RUN:   | FileCheck %s --check-prefixes=CHECK,ENTER
; RUN: llc -march=klausscpu -verify-machineinstrs -O1 -klausscpu-leaveret=false < %s \
; RUN:   | FileCheck %s --check-prefixes=CHECK,LEAVE
; RUN: llc -march=klausscpu -verify-machineinstrs -O1 -klausscpu-enter-leave=false < %s \
; RUN:   | FileCheck %s --check-prefixes=CHECK,PUSH

; Call frame information.  CFA = caller's SP before its CALL; the CIE starts
; each function at CFA = SP+8 with the return address at CFA-8.  After the
; prologue CFA = R15+16 and the caller's R15 is at CFA-16.

declare i64 @ext(i64)

; Leaf function with a frame and no callee-saved registers.
define i64 @leaf(i64 %x) uwtable {
; CHECK-LABEL: leaf:
; CHECK:         .cfi_startproc
; ENTER:         enter 1
; ENTER-NEXT:    .cfi_def_cfa r15, 16
; ENTER-NEXT:    .cfi_offset r15, -16
; PUSH:          push r15
; PUSH-NEXT:     .cfi_def_cfa_offset 16
; PUSH-NEXT:     .cfi_offset r15, -16
; PUSH-NEXT:     getsp r15
; PUSH-NEXT:     .cfi_def_cfa_register r15
; PUSH-NEXT:     addsp -8
; ENTER:         leaveret
; ENTER-NOT:     .cfi_
; LEAVE:         leave
; LEAVE-NEXT:    .cfi_def_cfa sp, 8
; LEAVE-NEXT:    .cfi_same_value r15
; LEAVE-NEXT:    ret
; PUSH:          setsp r15
; PUSH-NEXT:     .cfi_def_cfa_register sp
; PUSH-NEXT:     pop r15
; PUSH-NEXT:     .cfi_def_cfa_offset 8
; PUSH-NEXT:     .cfi_same_value r15
; PUSH-NEXT:     ret
; CHECK:         .cfi_endproc
  %slot = alloca i64
  store volatile i64 %x, ptr %slot
  %v = load volatile i64, ptr %slot
  ret i64 %v
}

; Callee-saved registers: .cfi_offset rules come after the spills, at
; R15-relative slot offset - 16; the frame teardown drops them again.
define i64 @csrs(i64 %a, i64 %b) uwtable {
; CHECK-LABEL: csrs:
; CHECK:         stidx64 r4, r15, -8
; CHECK-NEXT:    stidx64 r5, r15, -16
; CHECK-NEXT:    .cfi_offset r4, -24
; CHECK-NEXT:    .cfi_offset r5, -32
; CHECK:         ldidx64 r5, r15, -16
; CHECK:         ldidx64 r4, r15, -8
; ENTER:         leaveret
; LEAVE:         leave
; LEAVE-NEXT:    .cfi_def_cfa sp, 8
; LEAVE-NEXT:    .cfi_same_value r15
; LEAVE-NEXT:    .cfi_same_value r4
; LEAVE-NEXT:    .cfi_same_value r5
; PUSH:          .cfi_def_cfa_offset 8
; PUSH-NEXT:     .cfi_same_value r15
; PUSH-NEXT:     .cfi_same_value r4
; PUSH-NEXT:     .cfi_same_value r5
  %x = call i64 @ext(i64 %a)
  %y = call i64 @ext(i64 %b)
  %z = call i64 @ext(i64 %x)
  %s = add i64 %x, %y
  %r = add i64 %s, %z
  ret i64 %r
}

; No debug info and no unwind table: no CFI at all.
define i64 @no_cfi(i64 %x) nounwind {
; CHECK-LABEL: no_cfi:
; CHECK-NOT:     .cfi_
; CHECK:         .Lfunc_end2:
  %slot = alloca i64
  store volatile i64 %x, ptr %slot
  %v = load volatile i64, ptr %slot
  ret i64 %v
}
