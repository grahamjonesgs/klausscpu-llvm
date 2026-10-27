; A3a — arithmetic Z-flag reuse (post-RA peephole, KlaussCPUFlagReuse.cpp).
;
; When the branch condition is (X == 0) / (X != 0) and X is produced by a
; flag-setting arithmetic op immediately before the compare, the compare
; against 0 is redundant: the arithmetic op already set Z = (X == 0).
;
; Default (now ON post flag-unification): the redundant CMPRV against 0 is
; dropped and the equality branch reads Z via JMPZ/JMPNZ (either sense: block
; layout may invert it). The v3 fused compare-and-branch is disabled here so the
; unfused CMPRV + JMPcc form this pass works on is what reaches the output.
; With -klausscpu-arith-flag-reuse=false: the CMPRV is kept (JMPE/JMPNE) — the
; escape hatch retained for A/B measurement and bisection.
;
; RUN: llc -march=klausscpu -verify-machineinstrs -O1 -klausscpu-fuse-cmp-br=false < %s | FileCheck %s --check-prefix=ON
; RUN: llc -march=klausscpu -verify-machineinstrs -O1 -klausscpu-fuse-cmp-br=false -klausscpu-arith-flag-reuse=false < %s \
; RUN:   | FileCheck %s --check-prefix=OFF
; RUN: llc -march=klausscpu -verify-machineinstrs -O1 < %s | FileCheck %s --check-prefix=FUSE

; Branch on (a + b) == 0 — the ADDR result is tested against zero.
define i64 @add_eq0(i64 %a, i64 %b) {
; OFF-LABEL: add_eq0:
; OFF:         addr r{{[0-9]+}}, r0, r1
; OFF-NEXT:    cmprv r{{[0-9]+}}, 0
; OFF-NEXT:    jmp{{n?}}e{{(rel)?}} .LBB0_
;
; ON-LABEL: add_eq0:
; ON:         addr r{{[0-9]+}}, r0, r1
; ON-NEXT:    jmp{{n?}}z{{(rel)?}} .LBB0_
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
; OFF-NEXT:    jmp{{n?}}e{{(rel)?}} .LBB1_
;
; ON-LABEL: sub_ne0:
; ON:         subr r{{[0-9]+}}, r0, r1
; ON-NEXT:    jmp{{n?}}z{{(rel)?}} .LBB1_
; ON-NOT:     cmprv
  %s = sub i64 %a, %b
  %c = icmp ne i64 %s, 0
  br i1 %c, label %t, label %f
t:
  ret i64 %s
f:
  ret i64 42
}

; Negative: a signed comparison uses CMPRR + JMPGE (reads S^V), not the
; CMPRV-against-0 + JMPE/JMPNE the peephole targets — it must be left alone,
; and no Z-flag branch may appear in either mode.
define i64 @signed_lt_branch(i64 %a, i64 %b) {
; OFF-LABEL: signed_lt_branch:
; OFF:         cmprr r0, r1
; OFF-NEXT:    jmp{{ge|lt}}
; OFF-NOT:     jmpz
; OFF-NOT:     jmpnz
;
; ON-LABEL: signed_lt_branch:
; ON:         cmprr r0, r1
; ON-NEXT:    jmp{{ge|lt}}
; ON-NOT:     jmpz
; ON-NOT:     jmpnz
  %c = icmp slt i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 42
}

; Negative: if (d == 0) ... else if (d >= 0) ... else ...  MachineCSE drops the
; second `cmprv d, 0`, so the %nz block's jmplt reads the first compare's flags
; (FLAGS live-in). Dropping that compare would make jmplt read ADDR's S^V, which
; differs when the add wraps (d = 2^62 + 2^62 would take the d >= 0 path). The
; compare must stay.
define void @flags_live_into_successor(i64 %a, i64 %b, ptr %p) {
; OFF-LABEL: flags_live_into_successor:
; OFF:         addr r12, r0, r1
; OFF-NEXT:    cmprv r12, 0
; OFF-NEXT:    jmpe{{(rel)?}} .LBB3_
; OFF-NEXT:  # %bb.1:
; OFF-NEXT:    jmplt{{(rel)?}} .LBB3_
;
; ON-LABEL: flags_live_into_successor:
; ON:         addr r12, r0, r1
; ON-NEXT:    cmprv r12, 0
; ON-NEXT:    jmpe{{(rel)?}} .LBB3_
; ON-NEXT:  # %bb.1:
; ON-NEXT:    jmplt{{(rel)?}} .LBB3_
;
; With fusion on the pair is not fused either: the fused branch writes no flags.
; FUSE-LABEL: flags_live_into_successor:
; FUSE:         addr r12, r0, r1
; FUSE-NEXT:    cmprv r12, 0
; FUSE-NEXT:    jmpe{{(rel)?}} .LBB3_
; FUSE-NEXT:  # %bb.1:
; FUSE-NEXT:    jmplt{{(rel)?}} .LBB3_
entry:
  %d = add i64 %a, %b
  %z = icmp eq i64 %d, 0
  br i1 %z, label %iszero, label %nz
iszero:
  store volatile i64 1, ptr %p
  ret void
nz:
  %n = icmp sge i64 %d, 0
  br i1 %n, label %pos, label %neg
pos:
  %p1 = getelementptr i64, ptr %p, i64 1
  store volatile i64 2, ptr %p1
  ret void
neg:
  %p2 = getelementptr i64, ptr %p, i64 2
  store volatile i64 3, ptr %p2
  ret void
}

; Any op whose Z is (result == 0) qualifies, here a multiply and a shift (the
; CPU sets Z from the written-back value: see setsZeroFlagAsResult).  The
; branch then needs no compare even with fusion on, where it would otherwise
; be a fused FBR.
define i64 @mul_eq0(i64 %a, i64 %b) {
; OFF-LABEL: mul_eq0:
; OFF:         mulr r{{[0-9]+}}, r0, r1
; OFF-NEXT:    cmprv r{{[0-9]+}}, 0
; OFF-NEXT:    jmp{{n?}}e{{(rel)?}} .LBB4_
;
; ON-LABEL: mul_eq0:
; ON:         mulr r{{[0-9]+}}, r0, r1
; ON-NEXT:    jmp{{n?}}z{{(rel)?}} .LBB4_
; ON-NOT:     cmprv
;
; FUSE-LABEL: mul_eq0:
; FUSE:         mulr r{{[0-9]+}}, r0, r1
; FUSE-NEXT:    jmp{{n?}}z{{(rel)?}} .LBB4_
; FUSE-NOT:     fbr
  %m = mul i64 %a, %b
  %c = icmp eq i64 %m, 0
  br i1 %c, label %t, label %f
t:
  ret i64 42
f:
  ret i64 %m
}

define i64 @shl_ne0(i64 %a) {
; OFF-LABEL: shl_ne0:
; OFF:         shlv r{{[0-9]+}}, r0, 3
; OFF-NEXT:    cmprv r{{[0-9]+}}, 0
; OFF-NEXT:    jmp{{n?}}e{{(rel)?}} .LBB5_
;
; ON-LABEL: shl_ne0:
; ON:         shlv r{{[0-9]+}}, r0, 3
; ON-NEXT:    jmp{{n?}}z{{(rel)?}} .LBB5_
; ON-NOT:     cmprv
;
; FUSE-LABEL: shl_ne0:
; FUSE:         shlv r{{[0-9]+}}, r0, 3
; FUSE-NEXT:    jmp{{n?}}z{{(rel)?}} .LBB5_
; FUSE-NOT:     fbr
  %s = shl i64 %a, 3
  %c = icmp ne i64 %s, 0
  br i1 %c, label %t, label %f
t:
  ret i64 %s
f:
  ret i64 42
}
