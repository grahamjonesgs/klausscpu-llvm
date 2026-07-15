; A3a — arithmetic zero_flag reuse (post-RA peephole, KlaussCPUFlagReuse.cpp).
;
; When the branch condition is (X == 0) / (X != 0) and X is produced by a
; flag-setting arithmetic op immediately before the compare, the compare
; against 0 is redundant: the arithmetic op already set zero_flag = (X == 0).
;
; Default (now ON post flag-unification): the redundant CMPRV against 0 is
; dropped and the equality branch reads Z via JMPZ/JMPNZ.
; With -klausscpu-arith-flag-reuse=false: the CMPRV is kept (JMPE/JMPNE) — the
; escape hatch retained for A/B measurement and bisection.
;
; RUN: llc -march=klausscpu -O1 < %s | FileCheck %s --check-prefix=ON
; RUN: llc -march=klausscpu -O1 -klausscpu-arith-flag-reuse=false < %s \
; RUN:   | FileCheck %s --check-prefix=OFF

; Branch on (a + b) == 0 — the ADDR result is tested against zero.
define i64 @add_eq0(i64 %a, i64 %b) {
; OFF-LABEL: add_eq0:
; OFF:         addr r{{[0-9]+}}, r0, r1
; OFF-NEXT:    cmprv r{{[0-9]+}}, 0
; OFF-NEXT:    jmpne .LBB0_2
;
; ON-LABEL: add_eq0:
; ON:         addr r{{[0-9]+}}, r0, r1
; ON-NEXT:    jmpnz .LBB0_2
; ON-NOT:     cmprv
  %s = add i64 %a, %b
  %c = icmp eq i64 %s, 0
  br i1 %c, label %t, label %f
t:
  ret i64 %s
f:
  ret i64 42
}

; Branch on (a - b) != 0 — SUBR result tested against zero.
define i64 @sub_ne0(i64 %a, i64 %b) {
; OFF-LABEL: sub_ne0:
; OFF:         subr r{{[0-9]+}}, r0, r1
; OFF-NEXT:    cmprv r{{[0-9]+}}, 0
; OFF-NEXT:    jmpe .LBB1_2
;
; ON-LABEL: sub_ne0:
; ON:         subr r{{[0-9]+}}, r0, r1
; ON-NEXT:    jmpz .LBB1_2
; ON-NOT:     cmprv
  %s = sub i64 %a, %b
  %c = icmp ne i64 %s, 0
  br i1 %c, label %t, label %f
t:
  ret i64 %s
f:
  ret i64 42
}

; Negative: a signed comparison uses CMPRR + JMPGE (less_flag), not the
; CMPRV-against-0 + JMPE/JMPNE the peephole targets — it must be left alone,
; and no zero_flag branch may appear in either mode.
define i64 @signed_lt_branch(i64 %a, i64 %b) {
; OFF-LABEL: signed_lt_branch:
; OFF:         cmprr r0, r1
; OFF-NEXT:    jmpge
; OFF-NOT:     jmpz
; OFF-NOT:     jmpnz
;
; ON-LABEL: signed_lt_branch:
; ON:         cmprr r0, r1
; ON-NEXT:    jmpge
; ON-NOT:     jmpz
; ON-NOT:     jmpnz
  %c = icmp slt i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 42
}
