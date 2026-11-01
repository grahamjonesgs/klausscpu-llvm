; RUN: llc -march=klausscpu -verify-machineinstrs -O0 < %s | FileCheck %s
; RUN: llc -march=klausscpu -verify-machineinstrs -O2 < %s | FileCheck %s

; ABI version 3: the DataLayout has no i128 entry, so an i128 is 8-aligned,
; like the stack and malloc.  An i128 in memory no longer makes the function
; realign its frame (a 16-byte alignment reserved R7 and masked the frame
; pointer in the prologue), and a heap i128 is 8-aligned (clang/test/CodeGen/klausscpu-int128.c
; checks the DataLayout and the front end's view).

declare void @use(ptr)

; CHECK-LABEL: local_i128:
; CHECK-NOT:     andi
; CHECK-NOT:     r7
; CHECK:         callrel use
define void @local_i128() nounwind {
  %x = alloca i128
  store i128 1, ptr %x
  call void @use(ptr %x)
  ret void
}

; A load with the natural alignment is two 8-byte loads.
; CHECK-LABEL: natural_align:
; CHECK:         ldidx64 r12, r0, 0
; CHECK-NEXT:    ldidx64 r11, r0, 8
define i128 @natural_align(ptr %p) {
  %v = load i128, ptr %p
  ret i128 %v
}

; An explicit 16-byte alignment is still honoured, with the realignment.
; CHECK-LABEL: aligned16_i128:
; CHECK:         andi
define void @aligned16_i128() nounwind {
  %x = alloca i128, align 16
  store i128 1, ptr %x
  call void @use(ptr %x)
  ret void
}
