; RUN: llc -march=klausscpu -verify-machineinstrs -O2 < %s | FileCheck %s
; RUN: llc -march=klausscpu -verify-machineinstrs -O0 < %s | FileCheck %s --check-prefix=O0

; llvm.klausscpu.di / .ei (clang's __builtin_klausscpu_di / _ei): the core's
; interrupt-enable bit.  di returns the old bit, ei restores it.  A critical
; section keeps its memory accesses between the two instructions.

declare i64 @llvm.klausscpu.di()
declare void @llvm.klausscpu.ei(i64)

; The classic critical section: the load and the store stay inside, the old
; state goes back with ei, and nothing else is needed (no INT_MASK access).
; CHECK-LABEL: crit:
; CHECK:       di [[K:r[0-9]+]]
; CHECK-NEXT:  {{ldidx32|memget32}} {{.*}}
; CHECK:       {{stidx32|memset32}}
; CHECK-NEXT:  ei [[K]]
; CHECK-NOT:   0xF00F0000
; O0-LABEL: crit:
; O0:       di
; O0:       ei
define i32 @crit(ptr %p) {
  %k = call i64 @llvm.klausscpu.di()
  %v = load volatile i32, ptr %p
  %n = add i32 %v, 1
  store volatile i32 %n, ptr %p
  call void @llvm.klausscpu.ei(i64 %k)
  ret i32 %v
}

; Plain (non-volatile) accesses must not move across the intrinsics either:
; the intrinsics read and write memory.
; CHECK-LABEL: plain:
; CHECK:       {{stidx32|memset32}}
; CHECK:       di
; CHECK:       {{ldidx32|memget32}}
; CHECK:       ei
; CHECK:       {{stidx32|memset32}}
define i32 @plain(ptr %p, ptr %q) {
  store i32 1, ptr %q
  %k = call i64 @llvm.klausscpu.di()
  %v = load i32, ptr %p
  call void @llvm.klausscpu.ei(i64 %k)
  store i32 2, ptr %q
  ret i32 %v
}

; Two sections are not combined and neither is dropped when its result is
; unused.
; CHECK-LABEL: twice:
; CHECK:       di
; CHECK:       ei
; CHECK:       di
; CHECK:       ei
define void @twice() {
  %a = call i64 @llvm.klausscpu.di()
  call void @llvm.klausscpu.ei(i64 %a)
  %b = call i64 @llvm.klausscpu.di()
  call void @llvm.klausscpu.ei(i64 %b)
  ret void
}

; Enabling is a constant: ei of a constant 1 needs one register.
; CHECK-LABEL: enable:
; CHECK:       ei
define void @enable() {
  call void @llvm.klausscpu.ei(i64 1)
  ret void
}

; EI reads bit 0 only, so the zero-extension of a 32-bit key (clang's
; builtin takes and returns unsigned int) is dropped.
; CHECK-LABEL: key32:
; CHECK:       di [[K:r[0-9]+]]
; CHECK-NOT:   zextw
; CHECK-NOT:   andi
; CHECK:       ei [[K]]
define void @key32(ptr %p) {
  %d = call i64 @llvm.klausscpu.di()
  %k = trunc i64 %d to i32
  store volatile i32 1, ptr %p
  %z = zext i32 %k to i64
  call void @llvm.klausscpu.ei(i64 %z)
  ret void
}

; The result of DI feeding a sign extension: KlaussCPUOptExtensions looks at
; the instruction that defines the extended register, and DI has no source
; operand (it asserted on operand 1).
; CHECK-LABEL: sext_key:
; CHECK:       di
define i64 @sext_key() {
  %d = call i64 @llvm.klausscpu.di()
  %t = trunc i64 %d to i32
  %s = sext i32 %t to i64
  ret i64 %s
}
