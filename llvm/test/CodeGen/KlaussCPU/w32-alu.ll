; RUN: llc -march=klausscpu -verify-machineinstrs -O1 < %s | FileCheck %s
; RUN: llc -march=klausscpu -verify-machineinstrs -O1 -klausscpu-w32-alu=false < %s \
; RUN:   | FileCheck %s --check-prefix=OFF
;
; ISA v3 D3: a 32-bit add/sub/mul whose result is sign-extended to 64 bits
; (sext(i32 op) — C `int` arithmetic kept in a 64-bit register) selects the
; W form, which computes the low 32 bits and sign-extends in one op.
; -klausscpu-w32-alu=false keeps the 64-bit op + separate sign-extend.

define i64 @addw(i32 %a, i32 %b) {
; CHECK-LABEL: addw:
; CHECK: addw r12, r0, r1
; OFF-LABEL: addw:
; OFF: addr r12, r0, r1
; OFF-NEXT: sextw r12, r12
  %s = add i32 %a, %b
  %r = sext i32 %s to i64
  ret i64 %r
}

define i64 @subw(i32 %a, i32 %b) {
; CHECK-LABEL: subw:
; CHECK: subw r12, r0, r1
; OFF-LABEL: subw:
; OFF: subr r12, r0, r1
; OFF-NEXT: sextw r12, r12
  %s = sub i32 %a, %b
  %r = sext i32 %s to i64
  ret i64 %r
}

define i64 @mulw(i32 %a, i32 %b) {
; CHECK-LABEL: mulw:
; CHECK: mulw r12, r0, r1
; OFF-LABEL: mulw:
; OFF: mulr r12, r0, r1
; OFF-NEXT: sextw r12, r12
  %s = mul i32 %a, %b
  %r = sext i32 %s to i64
  ret i64 %r
}

define i64 @addiw(i32 %a) {
; CHECK-LABEL: addiw:
; CHECK: addiw r12, r0, 1000
; OFF-LABEL: addiw:
; OFF: addi r12, r0, 1000
; OFF-NEXT: sextw r12, r12
  %s = add i32 %a, 1000
  %r = sext i32 %s to i64
  ret i64 %r
}

; The W pattern only replaces an add/sub/mul with no other use: here the
; add is also stored.  The store reads only its low 32 bits, so
; KlaussCPUOptExtensions (rule 4) makes the add itself the ADDIW and the
; SEXTW goes; with -klausscpu-w32-alu=false the SEXTW stays.
@g = global i32 0

define i64 @addiw_multi_use(i32 %a) {
; CHECK-LABEL: addiw_multi_use:
; CHECK: addiw r12, r0, 1000
; CHECK-NOT: sextw
; CHECK: ret
; OFF-LABEL: addiw_multi_use:
; OFF: addi r14, r0, 1000
; OFF: sextw r12, r14
; OFF: ret
  %s = add i32 %a, 1000
  store i32 %s, ptr @g
  %r = sext i32 %s to i64
  ret i64 %r
}

; The register W forms take no constant operand: a constant left operand or
; a multiply by a constant keeps the 64-bit op and its SEXTW at isel.  A
; SUBR (MULV has no W form) then becomes SUBW through rule 4.
define i64 @subw_const_lhs(i32 %a) {
; CHECK-LABEL: subw_const_lhs:
; CHECK: setr r12, 5
; CHECK-NEXT: subw r12, r12, r0
; CHECK-NEXT: ret
; OFF-LABEL: subw_const_lhs:
; OFF: subr r12, r12, r0
; OFF-NEXT: sextw r12, r12
  %s = sub i32 5, %a
  %r = sext i32 %s to i64
  ret i64 %r
}

define i64 @mulw_const(i32 %a) {
; CHECK-LABEL: mulw_const:
; CHECK: mulv r12, r0, 12345
; CHECK-NEXT: sextw r12, r12
  %s = mul i32 %a, 12345
  %r = sext i32 %s to i64
  ret i64 %r
}
