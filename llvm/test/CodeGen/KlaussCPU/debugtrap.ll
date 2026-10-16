; RUN: llc -march=klausscpu -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -march=klausscpu -filetype=obj < %s | llvm-objdump -d - \
; RUN:   | FileCheck %s --check-prefix=OBJ

; __builtin_debugtrap() is the debugger breakpoint TRAP (v2 0x6C040000);
; __builtin_trap() stays HALT.

define void @dbg() nounwind {
; CHECK-LABEL: dbg:
; CHECK:         trap
; OBJ:           00 00 04 6c trap
  call void @llvm.debugtrap()
  ret void
}

define void @abort_here() nounwind {
; CHECK-LABEL: abort_here:
; CHECK:         halt
; OBJ:           00 00 01 6c halt
  call void @llvm.trap()
  unreachable
}

declare void @llvm.debugtrap()
declare void @llvm.trap()
