; RUN: llc -march=klausscpu -verify-machineinstrs -O2 < %s | FileCheck %s
; RUN: llc -march=klausscpu -verify-machineinstrs -O2 < %s \
; RUN:   | llvm-mc -triple=klausscpu -filetype=obj -o /dev/null

; Machine Copy Propagation forwarding a copy into the address register of an
; inline-asm memory operand asks the target for getPointerRegClass(), which
; the backend did not implement: "Target didn't implement getPointerRegClass!".
; Reduced from mbedtls_zeroize_and_free (TF-PSA-Crypto platform_util.c),
; whose zeroize barrier is asm volatile("" : : "m"(*(char (*)[len]) buf)):
; buf lives across the memset call, then goes to free() in the argument
; register.

@fp = internal constant ptr @memset, align 8

define void @zeroize_and_free(ptr %buf, i64 %len) nounwind {
; CHECK-LABEL: zeroize_and_free:
; CHECK:         #APP
; CHECK:         #NO_APP
entry:
  %c = icmp eq ptr %buf, null
  br i1 %c, label %done, label %clear

clear:
  %f = load volatile ptr, ptr @fp, align 8
  %r = tail call ptr %f(ptr nonnull %buf, i32 signext 0, i64 %len)
  tail call void asm sideeffect "", "*m,~{flags}"(ptr nonnull elementtype(i8) %buf)
  br label %done

done:
  tail call void @free(ptr %buf)
  ret void
}

declare void @free(ptr)
declare ptr @memset(ptr, i32 signext, i64)
