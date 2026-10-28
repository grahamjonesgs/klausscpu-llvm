; REQUIRES: klausscpu
;; LTO: lld infers EM_KLAUSSCPU and ELFCLASS32 (64-bit registers, 32-bit
;; addresses) from a KlaussCPU bitcode file, links bitcode with a native
;; object, and the LTO output carries the ABI version in e_flags like any
;; object the backend writes.

; RUN: rm -rf %t && split-file %s %t && cd %t
; RUN: llvm-as a.ll -o a.o
; RUN: llvm-as b.ll -o b.o
; RUN: llvm-mc -triple=klausscpu -filetype=obj native.s -o native.o
; RUN: ld.lld a.o b.o native.o -o out
; RUN: llvm-readobj -h out | FileCheck %s --check-prefix=HDR
; RUN: llvm-objdump -d --no-show-raw-insn out | FileCheck %s

; HDR:      Class: 32-bit
; HDR:      DataEncoding: LittleEndian
; HDR:      Machine: EM_KLAUSSCPU
; HDR:      Flags [ (0x2)

;; add3 (b.o) is inlined into _start across the modules and folded; native
;; is tail-called.
; CHECK:      <_start>:
; CHECK-NEXT:   setr r0, 42
; CHECK-NEXT:   jmprel {{.*}} <native>

;--- a.ll
target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64"
target triple = "klausscpu-unknown-elf"

declare i64 @add3(i64)
declare void @native(i64)

define void @_start() {
  %x = call i64 @add3(i64 39)
  call void @native(i64 %x)
  ret void
}

;--- b.ll
target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64"
target triple = "klausscpu-unknown-elf"

define i64 @add3(i64 %x) {
  %r = add i64 %x, 3
  ret i64 %r
}

;--- native.s
  .globl native
native:
  ret
