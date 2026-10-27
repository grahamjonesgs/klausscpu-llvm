; RUN: llc -march=klausscpu -verify-machineinstrs -O1 < %s | FileCheck %s
; RUN: llc -march=klausscpu -verify-machineinstrs -O0 < %s | FileCheck %s
; RUN: llc -march=klausscpu -verify-machineinstrs -O1 < %s \
; RUN:   | llvm-mc -triple=klausscpu -filetype=obj -o /dev/null

; Inline asm memory ('m', 'o') and symbolic ('i') operands.  A memory operand
; is a (base, offset) pair printed as `base, offset`, the operand syntax of
; ldidx*/stidx*, with a constant address offset folded in and a stack slot
; addressed from the frame register.  A symbolic immediate prints its symbol
; (plus any offset).  The 'm' constraint used to abort isel ("Could not match
; memory address") and a symbolic 'i' operand printed nothing.  The last RUN
; line checks that the output reassembles.

@arr = global [6 x i64] zeroinitializer

define i64 @load_m(ptr %p) nounwind {
; CHECK-LABEL: load_m:
; CHECK:         #APP
; CHECK-NEXT:    ldidx64 r{{[0-9]+}}, r0, 24
; CHECK:         #NO_APP
  %a = getelementptr inbounds i8, ptr %p, i64 24
  %v = call i64 asm sideeffect "ldidx64 $0, $1", "=r,*m"(ptr elementtype(i64) %a)
  ret i64 %v
}

define i64 @load_m_neg(ptr %p) nounwind {
; CHECK-LABEL: load_m_neg:
; CHECK:         #APP
; CHECK-NEXT:    ldidx64 r{{[0-9]+}}, r0, -16
; CHECK:         #NO_APP
  %a = getelementptr inbounds i8, ptr %p, i64 -16
  %v = call i64 asm sideeffect "ldidx64 $0, $1", "=r,*m"(ptr elementtype(i64) %a)
  ret i64 %v
}

define void @store_m(ptr %p, i64 %v) nounwind {
; CHECK-LABEL: store_m:
; CHECK:         #APP
; CHECK-NEXT:    stidx64 r1, r0, 40
; CHECK:         #NO_APP
  %a = getelementptr inbounds i8, ptr %p, i64 40
  call void asm sideeffect "stidx64 $1, $0", "=*m,r"(ptr elementtype(i64) %a, i64 %v)
  ret void
}

define i64 @load_o(ptr %p) nounwind {
; CHECK-LABEL: load_o:
; CHECK:         #APP
; CHECK-NEXT:    ldidx32 r{{[0-9]+}}, r0, 8
; CHECK:         #NO_APP
  %a = getelementptr inbounds i8, ptr %p, i64 8
  %v = call i64 asm sideeffect "ldidx32 $0, $1", "=r,*o"(ptr elementtype(i32) %a)
  ret i64 %v
}

; A stack slot: the frame index plus the folded offset becomes r15-relative.
declare void @use(ptr)

define i64 @mem_stack(i64 %v) nounwind {
; CHECK-LABEL: mem_stack:
; CHECK:         #APP
; CHECK-NEXT:    stidx64 r{{[0-9]+}}, r15, [[#%d,X:]]
; CHECK:         #APP
; CHECK-NEXT:    ldidx64 r{{[0-9]+}}, r15, [[#%d,X+16]]
; CHECK:         #NO_APP
  %x = alloca [4 x i64], align 8
  call void @use(ptr %x)
  call void asm sideeffect "stidx64 $1, $0", "=*m,r"(ptr elementtype(i64) %x, i64 %v)
  %a = getelementptr inbounds i8, ptr %x, i64 16
  %r = call i64 asm sideeffect "ldidx64 $0, $1", "=r,*m"(ptr elementtype(i64) %a)
  call void @use(ptr %x)
  ret i64 %r
}

; A global address is materialized into the base register; the constant
; offset folds into the operand's offset.
define i64 @load_m_global() nounwind {
; CHECK-LABEL: load_m_global:
; CHECK:         setru [[R:r[0-9]+]], arr{{$}}
; CHECK:         #APP
; CHECK-NEXT:    ldidx64 r{{[0-9]+}}, [[R]], 32
; CHECK:         #NO_APP
  %v = call i64 asm sideeffect "ldidx64 $0, $1", "=r,*m"(ptr elementtype(i64) getelementptr inbounds (i8, ptr @arr, i64 32))
  ret i64 %v
}

define void @sym_i() nounwind {
; CHECK-LABEL: sym_i:
; CHECK:         #APP
; CHECK-NEXT:    setr r1, arr{{$}}
; CHECK:         setr r1, arr+16
; CHECK:         setr r1, use
; CHECK:         setr r1, arr{{$}}
; CHECK:         #NO_APP
  call void asm sideeffect "setr r1, $0\0Asetr r1, $1\0Asetr r1, $2\0Asetr r1, ${0:c}", "i,i,i,~{r1}"(ptr @arr, ptr getelementptr inbounds (i8, ptr @arr, i64 16), ptr @use)
  ret void
}

define void @sym_blockaddress() nounwind {
; CHECK-LABEL: sym_blockaddress:
; CHECK:         #APP
; CHECK-NEXT:    setr r1, .Ltmp{{[0-9]+}}
; CHECK:         #NO_APP
entry:
  call void asm sideeffect "setr r1, $0", "i,~{r1}"(ptr blockaddress(@sym_blockaddress, %target))
  br label %target
target:
  ret void
}

; The generic '%a' modifier on a register operand prints it as a zero-offset
; address.  It used to read the next operand's flag word as the offset
; (silently wrong code), or assert when the register was the last operand.
define i64 @amod_reg_last(ptr %p) nounwind {
; CHECK-LABEL: amod_reg_last:
; CHECK:         #APP
; CHECK-NEXT:    ldidx64 r{{[0-9]+}}, r0, 0{{$}}
; CHECK:         #NO_APP
  %v = call i64 asm sideeffect "ldidx64 $0, ${1:a}", "=r,r"(ptr %p)
  ret i64 %v
}

define i64 @amod_reg_mid(ptr %p, i64 %q) nounwind {
; CHECK-LABEL: amod_reg_mid:
; CHECK:         #APP
; CHECK-NEXT:    ldidx64 r{{[0-9]+}}, r0, 0{{$}}
; CHECK:         #NO_APP
  %v = call i64 asm sideeffect "ldidx64 $0, ${1:a}", "=r,r,r"(ptr %p, i64 %q)
  ret i64 %v
}

define i64 @amod_reg_clobber(ptr %p) nounwind {
; CHECK-LABEL: amod_reg_clobber:
; CHECK:         #APP
; CHECK-NEXT:    ldidx64 r{{[0-9]+}}, r0, 0{{$}}
; CHECK:         #NO_APP
  %v = call i64 asm sideeffect "ldidx64 $0, ${1:a}", "=r,r,~{r3}"(ptr %p)
  ret i64 %v
}

; A constant is folded onto a stack slot only while |constant| < 512 MiB,
; so the folded offset cannot leave simm32.  A larger one stays in the address
; register.  It used to fold any simm32: -2147483648 wrapped to
; `r15, -2147483680`, which encodes as +2147483616, and with every GPR
; clobbered by the asm, eliminateFrameIndex could not scavenge a register for
; the slot address and llc aborted.
define i64 @mem_stack_fold_limit(i64 %v) nounwind {
; CHECK-LABEL: mem_stack_fold_limit:
; CHECK:         #APP
; CHECK-NEXT:    ldidx64 r{{[0-9]+}}, r15, -5368709{{[0-9][0-9]$}}
; CHECK:         #NO_APP
  %x = alloca [4 x i64], align 8
  call void @use(ptr %x)
  %a = getelementptr i8, ptr %x, i64 -536870912
  %r = call i64 asm sideeffect "ldidx64 $0, $1", "=r,*m"(ptr elementtype(i64) %a)
  ret i64 %r
}

define i64 @mem_stack_past_limit(i64 %v) nounwind {
; CHECK-LABEL: mem_stack_past_limit:
; CHECK:         addi [[A:r[0-9]+]], r{{[0-9]+}}, 536870912
; CHECK-NEXT:    #APP
; CHECK-NEXT:    ldidx64 r{{[0-9]+}}, [[A]], 0
; CHECK:         #NO_APP
  %x = alloca [4 x i64], align 8
  call void @use(ptr %x)
  %a = getelementptr i8, ptr %x, i64 536870912
  %r = call i64 asm sideeffect "ldidx64 $0, $1", "=r,*m"(ptr elementtype(i64) %a)
  ret i64 %r
}

define i64 @mem_stack_far(i64 %v) nounwind {
; CHECK-LABEL: mem_stack_far:
; CHECK:         call use
; CHECK:         addi [[A:r[0-9]+]], r{{[0-9]+}}, -2147483648
; CHECK-NEXT:    #APP
; CHECK-NEXT:    ldidx64 r{{[0-9]+}}, [[A]], 0
; CHECK:         #NO_APP
  %x = alloca [4 x i64], align 8
  call void @use(ptr %x)
  %a = getelementptr i8, ptr %x, i64 -2147483648
  %r = call i64 asm sideeffect "ldidx64 $0, $1", "=r,*m"(ptr elementtype(i64) %a)
  ret i64 %r
}

define i64 @mem_stack_far_clobber(i64 %v) nounwind {
; CHECK-LABEL: mem_stack_far_clobber:
; CHECK:         call use
; CHECK:         addi [[A:r[0-9]+]], r{{[0-9]+}}, -2147483648
; CHECK-NEXT:    #APP
; CHECK-NEXT:    ldidx64 r{{[0-9]+}}, [[A]], 0
; CHECK:         #NO_APP
  %x = alloca [4 x i64], align 8
  call void @use(ptr %x)
  %a = getelementptr i8, ptr %x, i64 -2147483648
  %r = call i64 asm sideeffect "ldidx64 $0, $1", "=r,*m,~{r0},~{r1},~{r2},~{r3},~{r4},~{r5},~{r6},~{r7},~{r8},~{r9},~{r10},~{r11},~{r13},~{r14}"(ptr elementtype(i64) %a)
  ret i64 %r
}
