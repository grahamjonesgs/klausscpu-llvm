; RUN: not llc -march=klausscpu -verify-machineinstrs < %s -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --implicit-check-not=error: --implicit-check-not=warning:

; Inline asm and the frame registers.  In a function that realigns its stack,
; R7 is the base pointer that addresses the locals, so an asm operand that
; writes it is an error and a clobber of it draws the reserved-register
; warning (both used to overwrite the base pointer silently).  R15 (the frame
; pointer) is reserved in every function.  Elsewhere R7 is an ordinary
; callee-saved register.

declare void @use(ptr)

; CHECK: error: write to reserved register 'R7'
define i64 @realign_write_r7(i64 %a) nounwind {
  %b = alloca [8 x i8], align 32
  call void @use(ptr %b)
  %x = call i64 asm sideeffect "addi $0, $0, 1", "={r7},0"(i64 %a)
  ret i64 %x
}

; CHECK: error: write to reserved register 'R7'
define void @realign_read_r7(i64 %a) nounwind {
  %b = alloca [8 x i8], align 32
  call void @use(ptr %b)
  call void asm sideeffect "# $0", "{r7}"(i64 %a)
  ret void
}

; CHECK: warning: inline asm clobber list contains reserved registers: R7
define i8 @realign_clobber_r7() nounwind {
  %b = alloca [8 x i8], align 32
  call void @use(ptr %b)
  call void asm sideeffect "setr r7, 0", "~{r7}"()
  %v = load i8, ptr %b
  ret i8 %v
}

; CHECK: error: write to reserved register 'R15'
define i64 @write_r15(i64 %a) nounwind {
  %x = call i64 asm sideeffect "addi $0, $0, 1", "={r15},0"(i64 %a)
  ret i64 %x
}

; CHECK: warning: inline asm clobber list contains reserved registers: R15
define void @clobber_r15() nounwind {
  call void asm sideeffect "setr r15, 0", "~{r15}"()
  ret void
}

; No diagnostic: this function does not realign, so R7 is free.
define i64 @no_realign_r7(i64 %a) nounwind {
  %b = alloca [8 x i8], align 8
  call void @use(ptr %b)
  %x = call i64 asm sideeffect "addi $0, $0, 1", "={r7},0"(i64 %a)
  call void asm sideeffect "setr r7, 0", "~{r7}"()
  ret i64 %x
}
