; RUN: llc -march=klausscpu -verify-machineinstrs -O2 < %s | FileCheck %s
; RUN: llc -march=klausscpu -verify-machineinstrs -O2 -relocation-model=pic < %s \
; RUN:   | FileCheck %s --check-prefix=PIC
; RUN: llc -march=klausscpu -O2 -filetype=obj < %s -o /dev/null
;
; blockaddress / computed goto (`&&label`, `goto *p`).  The AsmPrinter used to
; hit llvm_unreachable on MO_BlockAddress; blocks ending in JMPR_R used to be
; reported by analyzeBranch as falling through (fails the machine verifier).

define ptr @addr_of_label() {
; CHECK-LABEL: addr_of_label:
; CHECK:         setr r12, .Ltmp0
; PIC-LABEL: addr_of_label:
; PIC:           leapc r12, .Ltmp0
entry:
  br label %l
l:
  ret ptr blockaddress(@addr_of_label, %l)
}

define i64 @computed_goto(ptr %t, i64 %i, i64 %x) {
; CHECK-LABEL: computed_goto:
; CHECK:         ldidx64r [[T:r[0-9]+]], r0, {{r[0-9]+}}
; CHECK-NEXT:    jmpr [[T]]
entry:
  %c = icmp eq i64 %x, 0
  br i1 %c, label %z, label %go
go:
  %p = getelementptr ptr, ptr %t, i64 %i
  %d = load ptr, ptr %p
  indirectbr ptr %d, [label %a, label %b]
a:
  ret i64 1
b:
  ret i64 2
z:
  ret i64 3
}
