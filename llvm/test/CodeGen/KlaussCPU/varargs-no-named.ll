; RUN: not llc -march=klausscpu -O1 < %s -o /dev/null 2>&1 | FileCheck %s

; The caller reserves home slots for R1-R3 only (R0's would be the return
; address), so a variadic function with no named parameter (C23 f(...))
; cannot be compiled.  This used to store R0 over the return address.

; CHECK: error: {{.*}}in function no_named i64 (...): variadic functions without a named parameter are not supported by the KlaussCPU calling convention

define i64 @no_named(...) {
  %ap = alloca ptr
  call void @llvm.va_start.p0(ptr %ap)
  %v = va_arg ptr %ap, i64
  call void @llvm.va_end.p0(ptr %ap)
  ret i64 %v
}

; Calling one is fine.
define i64 @caller() {
  %r = call i64 (...) @no_named(i64 1, i64 2)
  ret i64 %r
}
