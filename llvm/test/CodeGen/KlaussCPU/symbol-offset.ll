; RUN: llc -mtriple=klausscpu -verify-machineinstrs -O1 < %s | FileCheck %s
; RUN: llc -mtriple=klausscpu -verify-machineinstrs -O0 < %s \
; RUN:   | FileCheck %s --check-prefix=O0
; RUN: llc -mtriple=klausscpu -verify-machineinstrs -O1 -relocation-model=pic < %s \
; RUN:   | FileCheck %s --check-prefix=PIC
;
; SETRU (static) and LEAPC (PIC) build a symbol address zero-extended, so
; `sym+off` folded into one is exact only while S + off stays in [0, 2^32).
; A negative offset can take it below zero: `&arr[i - 100000]` with arr at a
; low address must be the negative 64-bit value, not 0x00000000_FFFxxxxx,
; or the index add carries into bit 32 and pointer compares / ptrtoint go
; wrong.  Only an offset in [0, 2^31) is folded into the symbol; any other is
; a separate add.  (Under PIC the generic code never folds an offset.)

@arr = dso_local global [100 x i64] zeroinitializer

; &arr[i - 100000]
define ptr @neg_index(i64 %i) nounwind {
; CHECK-LABEL: neg_index:
; CHECK-NOT:     arr-
; CHECK:         setru [[A:r[0-9]+]], arr{{$}}
; CHECK:         addi {{r[0-9]+}}, {{r[0-9]+}}, -800000
; CHECK:         leaveret
; O0-LABEL: neg_index:
; O0-NOT:      arr-
; O0:          setru {{r[0-9]+}}, arr{{$}}
; O0:          leaveret
; PIC-LABEL: neg_index:
; PIC-NOT:     arr-
; PIC:         leapc {{r[0-9]+}}, arr{{$}}
; PIC:         leaveret
  %k = add i64 %i, -100000
  %p = getelementptr inbounds i64, ptr @arr, i64 %k
  ret ptr %p
}

; arr[i - 100000]: the offset still folds into the load's displacement.
define i64 @neg_index_load(i64 %i) nounwind {
; CHECK-LABEL: neg_index_load:
; CHECK-NOT:     arr-
; CHECK:         setru [[A:r[0-9]+]], arr{{$}}
; CHECK:         ldidx64 {{r[0-9]+}}, {{r[0-9]+}}, -800000
; CHECK-NEXT:    leaveret
  %k = add i64 %i, -100000
  %p = getelementptr inbounds i64, ptr @arr, i64 %k
  %v = load i64, ptr %p
  ret i64 %v
}

; (intptr_t)&arr - 0x100000 must be negative when arr is below 1 MiB.
define i64 @neg_int() nounwind {
; CHECK-LABEL: neg_int:
; CHECK:         setru [[R:r[0-9]+]], arr{{$}}
; CHECK-NEXT:    addi [[R]], [[R]], -1048576
; CHECK-NEXT:    leaveret
; O0-LABEL: neg_int:
; O0-NOT:      arr-
; O0:          leaveret
  %a = ptrtoint ptr @arr to i64
  %r = sub i64 %a, 1048576
  ret i64 %r
}

; A non-negative offset below 2^31 stays folded.
define i64 @pos_int() nounwind {
; CHECK-LABEL: pos_int:
; CHECK:         setru [[R:r[0-9]+]], arr+40{{$}}
; CHECK-NEXT:    leaveret
  %a = ptrtoint ptr @arr to i64
  %r = add i64 %a, 40
  ret i64 %r
}

; An offset of 2^31 or more is not (it is not an ELFCLASS32 addend).
define i64 @big_int() nounwind {
; CHECK-LABEL: big_int:
; CHECK:         setru [[R:r[0-9]+]], arr{{$}}
; CHECK-NEXT:    addv [[R]], [[R]], 2147483648
; CHECK-NEXT:    leaveret
  %a = ptrtoint ptr @arr to i64
  %r = add i64 %a, 2147483648
  ret i64 %r
}
