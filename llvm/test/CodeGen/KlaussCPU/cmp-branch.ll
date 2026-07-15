; Condition-code → branch-mnemonic mapping, pinned after the flag-model
; unification (RTL fbb77d7 / FLAG_UNIFICATION_CHANGES).
;
; The hardware now has ONE Z/S/C/V flags register and DERIVES each COND from it
; (EQ=Z, signed LT=S^V, unsigned ULT=C, ...).  The backend's job is only to pick
; the right COND mnemonic; the derivation (and the x86 BORROW polarity, ULT=C /
; UGE=¬C) lives in RTL and is proven bit-identical to the retired E/L/U flags by
; tb_flags.sv.  This test locks the codegen half of that contract: each ISD
; CondCode must select the correct JMP** (and its REL variant under PIC).
;
; NOTE on the mnemonics below: each function's taken block returns %a and is laid
; out as the fall-through, so LLVM emits the *negated* condition to the exit
; block.  That negation is itself part of the mapping table, so pinning it still
; catches any swap (e.g. ult<->uge, the borrow-polarity landmine).
;
; RUN: llc -march=klausscpu -O1 < %s | FileCheck %s
; RUN: llc -march=klausscpu -O1 -relocation-model=pic < %s \
; RUN:   | FileCheck %s --check-prefix=PIC

; ---- reg-reg compares (CMPRR) --------------------------------------------

define i64 @rr_eq(i64 %a, i64 %b) {
; CHECK-LABEL: rr_eq:
; CHECK:         cmprr r0, r1
; CHECK-NEXT:    jmpne
; PIC-LABEL: rr_eq:
; PIC:           cmprr r0, r1
; PIC-NEXT:      jmpnerel
  %c = icmp eq i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 42
}

define i64 @rr_ne(i64 %a, i64 %b) {
; CHECK-LABEL: rr_ne:
; CHECK:         cmprr r0, r1
; CHECK-NEXT:    jmpe
; PIC-LABEL: rr_ne:
; PIC:           cmprr r0, r1
; PIC-NEXT:      jmperel
  %c = icmp ne i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 42
}

define i64 @rr_slt(i64 %a, i64 %b) {
; CHECK-LABEL: rr_slt:
; CHECK:         cmprr r0, r1
; CHECK-NEXT:    jmpge
; PIC-LABEL: rr_slt:
; PIC:           cmprr r0, r1
; PIC-NEXT:      jmpgerel
  %c = icmp slt i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 42
}

define i64 @rr_sle(i64 %a, i64 %b) {
; CHECK-LABEL: rr_sle:
; CHECK:         cmprr r0, r1
; CHECK-NEXT:    jmpgt
  %c = icmp sle i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 42
}

define i64 @rr_sgt(i64 %a, i64 %b) {
; CHECK-LABEL: rr_sgt:
; CHECK:         cmprr r0, r1
; CHECK-NEXT:    jmple
  %c = icmp sgt i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 42
}

define i64 @rr_sge(i64 %a, i64 %b) {
; CHECK-LABEL: rr_sge:
; CHECK:         cmprr r0, r1
; CHECK-NEXT:    jmplt
  %c = icmp sge i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 42
}

; The four unsigned forms are the borrow-polarity guard (ULT=C, UGE=¬C).
define i64 @rr_ult(i64 %a, i64 %b) {
; CHECK-LABEL: rr_ult:
; CHECK:         cmprr r0, r1
; CHECK-NEXT:    jmpuge
; PIC-LABEL: rr_ult:
; PIC:           cmprr r0, r1
; PIC-NEXT:      jmpugerel
  %c = icmp ult i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 42
}

define i64 @rr_ule(i64 %a, i64 %b) {
; CHECK-LABEL: rr_ule:
; CHECK:         cmprr r0, r1
; CHECK-NEXT:    jmpugt
  %c = icmp ule i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 42
}

define i64 @rr_ugt(i64 %a, i64 %b) {
; CHECK-LABEL: rr_ugt:
; CHECK:         cmprr r0, r1
; CHECK-NEXT:    jmpule
  %c = icmp ugt i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 42
}

define i64 @rr_uge(i64 %a, i64 %b) {
; CHECK-LABEL: rr_uge:
; CHECK:         cmprr r0, r1
; CHECK-NEXT:    jmpult
; PIC-LABEL: rr_uge:
; PIC:           cmprr r0, r1
; PIC-NEXT:      jmpultrel
  %c = icmp uge i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 42
}

; ---- reg-imm compares (CMPRV), nonzero constant --------------------------

define i64 @rv_ult(i64 %a) {
; CHECK-LABEL: rv_ult:
; CHECK:         cmprv r0, 6
; CHECK-NEXT:    jmpugt
  %c = icmp ult i64 %a, 7
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 42
}

define i64 @rv_uge(i64 %a) {
; CHECK-LABEL: rv_uge:
; CHECK:         cmprv r0, 7
; CHECK-NEXT:    jmpult
  %c = icmp uge i64 %a, 7
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 42
}
