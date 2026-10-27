; RUN: llc -march=klausscpu -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -march=klausscpu -filetype=obj < %s | llvm-objdump -d - \
; RUN:   | FileCheck %s --check-prefix=OBJ
; RUN: llc -march=klausscpu -verify-machineinstrs -trap-unreachable < %s \
; RUN:   | FileCheck %s --check-prefix=UNR

; __builtin_debugtrap() and __builtin_trap() are both TRAP (v2 0x6C040000):
; the core parks and the SoC writes the crash dump (a debug halt under the
; debugger; `stop = Trap` in the emulator).  HALT (0x6C010000) is the clean
; stop the runtime ends a program with; no IR intrinsic selects it.

define void @dbg() nounwind {
; CHECK-LABEL: dbg:
; CHECK:         trap
; OBJ:           00 00 04 6c trap
  call void @llvm.debugtrap()
  ret void
}

define void @abort_here() nounwind {
; CHECK-LABEL: abort_here:
; CHECK:         trap
; CHECK-NOT:     halt
; OBJ:           00 00 04 6c trap
; OBJ-NOT:       halt
  call void @llvm.trap()
  unreachable
}

; -trap-unreachable lowers an `unreachable` to the same TRAP.
define void @unreach() nounwind {
; UNR-LABEL: unreach:
; UNR:         trap
; UNR-NOT:     halt
  unreachable
}

declare void @llvm.debugtrap()
declare void @llvm.trap()
