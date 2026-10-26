; An inline asm "~{flags}" or "~{cc}" clobber makes the INLINEASM define FLAGS,
; so a compare is never scheduled across the asm into a compare->branch gap.
; clang adds "~{flags}" to every asm statement (most KlaussCPU ALU ops set the
; flags).  The clobber used to be dropped: isel, the pre-RA scheduler and the
; post-RA scheduler all put the compare before an asm that writes the flags,
; and the branch then tested the asm's flags.
;
; RUN: llc -march=klausscpu -verify-machineinstrs -O0 < %s | FileCheck %s
; RUN: llc -march=klausscpu -verify-machineinstrs -O1 < %s | FileCheck %s
; RUN: llc -march=klausscpu -verify-machineinstrs -O2 < %s | FileCheck %s
; RUN: llc -march=klausscpu -O1 -stop-after=finalize-isel < %s \
; RUN:   | FileCheck %s --check-prefix=MIR

; The compare's operands are ready before the asm, which comes first.
define i64 @flags_before_cmp(i64 %x, i64 %y, i64 %a, i64 %b) nounwind {
; CHECK-LABEL: flags_before_cmp:
; CHECK:         #APP
; CHECK-NEXT:    addr {{r[0-9]+}}, r2, r3
; CHECK:         #NO_APP
; CHECK-NOT:     #APP
; CHECK:         cmprr r0, r1
; CHECK-NEXT:    jmp
; MIR-LABEL:   name: flags_before_cmp
; MIR:         INLINEASM &"addr $0, $1, $2"
; MIR-SAME:      implicit-def early-clobber $flags
entry:
  %t = call i64 asm "addr $0, $1, $2", "=r,r,r,~{flags}"(i64 %a, i64 %b)
  %c = icmp slt i64 %x, %y
  br i1 %c, label %t1, label %f1
t1:
  ret i64 %t
f1:
  %z = add i64 %t, 5
  ret i64 %z
}

; The compare comes first in the IR; it must still not be separated from the
; branch by the asm.
define i64 @cc_after_cmp(ptr %p, i64 %x, i64 %y, i64 %a) nounwind {
; CHECK-LABEL: cc_after_cmp:
; CHECK:         #APP
; CHECK-NEXT:    subr
; CHECK:         #NO_APP
; CHECK-NOT:     #APP
; CHECK:         cmprr r1, r2
; CHECK-NEXT:    jmp
; MIR-LABEL:   name: cc_after_cmp
; MIR:         INLINEASM &"subr $0, $1, $1"
; MIR-SAME:      implicit-def early-clobber $flags
entry:
  %v = load i64, ptr %p
  %c = icmp slt i64 %x, %y
  %t = call i64 asm sideeffect "subr $0, $1, $1", "=r,r,~{cc}"(i64 %v)
  br i1 %c, label %t1, label %f1
t1:
  ret i64 %t
f1:
  %z = add i64 %t, 5
  ret i64 %z
}
