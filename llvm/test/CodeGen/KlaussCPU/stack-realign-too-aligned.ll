; RUN: not llc -march=klausscpu < %s -o /dev/null 2>&1 | FileCheck %s

; The base pointer is R15 & -Alignment, which cannot be formed in the 32-bit
; address space for an alignment of 2^32 (the IR maximum); the mask would be 0.

; CHECK: LLVM ERROR: KlaussCPU: stack object alignment exceeds the 32-bit address space

declare void @use(ptr)

define void @f() nounwind {
  %x = alloca i8, align 4294967296
  call void @use(ptr %x)
  ret void
}
