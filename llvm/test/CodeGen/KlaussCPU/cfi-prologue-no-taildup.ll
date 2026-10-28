; RUN: llc -mtriple=klausscpu -O3 -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -mtriple=klausscpu -O3 -filetype=obj < %s -o %t.o
; RUN: llvm-dwarfdump --debug-frame %t.o | FileCheck %s --check-prefix=FRAME
; RUN: llc -mtriple=klausscpu -O3 -klausscpu-enter-leave=false \
; RUN:   -verify-machineinstrs < %s | FileCheck %s --check-prefix=PUSH
;
; A shrink-wrapped prologue in a small join block is not tail-duplicated.
; At -O3 block placement's tail duplication (4 instructions) used to copy the
; block holding `enter; stidx64 r4; fbri` (or `enter; fbri`, or
; `push r15; getsp r15; fbri`) into both arms of the select diamond in front
; of it.  The unwind info assumes one prologue that dominates the framed code
; (CFIFixup takes the last FrameSetup CFI in layout as the prologue;
; KlaussCPUCalleeSavedCFI gives each callee-saved register one rule), so the
; frameless code in front of the second ENTER was described as framed, the
; framed code after the first one as frameless (CFIFixup's reset), and the
; second R4 spill had no rule.  ENTER and PUSH are isNotDuplicable: the
; diamond keeps a jump to the one prologue.
;
; Generated from (clang -O3; nounwind dropped so that CFI is emitted):
;   long g(long);
;   long td(long a, long b, long c) {
;     long x = a ? b + 1 : c * 3; long r;
;     if (x > 5) { long t = g(x); r = t + g(t); }
;     else { long t = g(x + 1); r = t * g(t); }
;     return r; }
;   long nocsr(long a, long b, long c) {
;     long x = a ? b + 1 : c * 3;
;     return x > 5 ? g(x) + 7 : g(x + 1) * 3; }

; CHECK-LABEL: td:
; CHECK:         fbri r0, 0, 8, .LBB0_2
; CHECK-NEXT:  # %bb.1:
; CHECK-NEXT:    mulv r0, r2, 3
; CHECK-NEXT:    jmprel .LBB0_3
; CHECK-NEXT:  .LBB0_2:
; CHECK-NEXT:    incr r0, r1
; CHECK-NEXT:  .LBB0_3:
; CHECK-NEXT:    enter 1
; CHECK-NEXT:    .cfi_def_cfa r15, 16
; CHECK-NEXT:    .cfi_offset r15, -16
; CHECK-NEXT:    stidx64 r4, r15, -8
; CHECK-NEXT:    .cfi_offset r4, -24
; CHECK-NEXT:    fbri r0, 6, 1, .LBB0_5
; CHECK-NOT:     enter
; CHECK-NOT:     .cfi_def_cfa sp
; CHECK:         .cfi_endproc
;
; CHECK-LABEL: nocsr:
; CHECK:         jmprel .LBB1_3
; CHECK:       .LBB1_3:
; CHECK-NEXT:    enter 0
; CHECK-NOT:     enter
; CHECK:         .cfi_endproc

; One row per state: frameless up to the ENTER, framed after it to the end
; (the arm after the first LEAVERET is entered framed).
; FRAME:      FDE cie=00000000 pc=00000000...0000005c
; FRAME:      0x0: CFA=SP+8: RA=[CFA-8]
; FRAME-NEXT: 0x18: CFA=R15+16: R15=[CFA-16], RA=[CFA-8]
; FRAME-NEXT: 0x1c: CFA=R15+16: R4=[CFA-24], R15=[CFA-16], RA=[CFA-8]
; FRAME-EMPTY:
; FRAME-NEXT: FDE cie=00000000 pc=0000005c...00000098
; FRAME:      0x5c: CFA=SP+8: RA=[CFA-8]
; FRAME-NEXT: 0x74: CFA=R15+16: R15=[CFA-16], RA=[CFA-8]
; FRAME-EMPTY:

; PUSH-LABEL: nocsr:
; PUSH:         jmprel .LBB1_3
; PUSH:       .LBB1_3:
; PUSH-NEXT:    push r15
; PUSH-NOT:     push
; PUSH:         .cfi_endproc

declare i64 @g(i64)

define i64 @td(i64 %a, i64 %b, i64 %c) {
entry:
  %tobool.not = icmp eq i64 %a, 0
  %add = add nsw i64 %b, 1
  %mul = mul nsw i64 %c, 3
  %cond = select i1 %tobool.not, i64 %mul, i64 %add
  %cmp = icmp sgt i64 %cond, 5
  br i1 %cmp, label %if.then, label %if.else

if.then:
  %call = tail call i64 @g(i64 %cond)
  %call1 = tail call i64 @g(i64 %call)
  %add2 = add nsw i64 %call1, %call
  br label %if.end

if.else:
  %add4 = add nsw i64 %cond, 1
  %call5 = tail call i64 @g(i64 %add4)
  %call6 = tail call i64 @g(i64 %call5)
  %mul7 = mul nsw i64 %call6, %call5
  br label %if.end

if.end:
  %r.0 = phi i64 [ %add2, %if.then ], [ %mul7, %if.else ]
  ret i64 %r.0
}

define i64 @nocsr(i64 %a, i64 %b, i64 %c) {
entry:
  %tobool.not = icmp eq i64 %a, 0
  %add = add nsw i64 %b, 1
  %mul = mul nsw i64 %c, 3
  %cond = select i1 %tobool.not, i64 %mul, i64 %add
  %cmp = icmp sgt i64 %cond, 5
  br i1 %cmp, label %if.then, label %if.else

if.then:
  %call = call i64 @g(i64 %cond)
  %add2 = add nsw i64 %call, 7
  br label %if.end

if.else:
  %add4 = add nsw i64 %cond, 1
  %call5 = call i64 @g(i64 %add4)
  %mul7 = mul nsw i64 %call5, 3
  br label %if.end

if.end:
  %r.0 = phi i64 [ %add2, %if.then ], [ %mul7, %if.else ]
  ret i64 %r.0
}
