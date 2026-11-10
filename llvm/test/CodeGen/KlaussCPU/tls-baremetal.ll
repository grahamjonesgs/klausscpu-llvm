; RUN: not --crash llc -mtriple=klausscpu-unknown-elf < %s 2>&1 | FileCheck %s

; Bare metal has no thread pointer: a TLS access is an error, not code that
; reads TP.
; CHECK: LLVM ERROR: thread-local storage is supported on KlaussCPU only for Linux

@x = thread_local global i32 0

define i32 @f() {
  %v = load i32, ptr @x
  ret i32 %v
}
