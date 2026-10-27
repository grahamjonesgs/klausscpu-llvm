; RUN: llc -march=klausscpu -verify-machineinstrs -O2 -stop-after=finalize-isel < %s \
; RUN:   | FileCheck %s

; Select grouping at the point it happens (the SELECT_CC custom inserter):
; instructions that sit inside a run of same-condition selects but are not
; selects stay in the head block ahead of the single compare and branch, and
; each select becomes one PHI in the join block.  ADDR clobbers FLAGS (dead)
; and CMPULTRW writes none; neither ends the run.

define i64 @add_between(i64 %a, i64 %b, i64 %c, i64 %d, i64 %x) nounwind {
; CHECK-LABEL: name: add_between
; CHECK:       {{^  }}bb.0 (
; CHECK:         [[X:%[0-9]+]]:gpr = LDIDX64 %fixed-stack.0, 0
; CHECK-NEXT:    [[T:%[0-9]+]]:gpr = ADDR [[C:%[0-9]+]], [[X]], implicit-def dead $flags
; CHECK-NEXT:    CMPRR_I [[A:%[0-9]+]], [[B:%[0-9]+]], implicit-def $flags
; CHECK-NEXT:    JMPLT %bb.2, implicit $flags
; CHECK-NOT:     CMPRR_I
; CHECK:       {{^  }}bb.2 (
; CHECK-NEXT:    [[S1:%[0-9]+]]:gpr = PHI [[C]], %bb.0, {{%[0-9]+}}, %bb.1
; CHECK-NEXT:    [[S2:%[0-9]+]]:gpr = PHI [[T]], %bb.0, [[X]], %bb.1
; CHECK-NEXT:    MULR killed [[S1]], killed [[S2]]
  %cc = icmp slt i64 %a, %b
  %s1 = select i1 %cc, i64 %c, i64 %d
  %t = add i64 %c, %x
  %s2 = select i1 %cc, i64 %t, i64 %x
  %r = mul i64 %s1, %s2
  ret i64 %r
}

define i64 @wbool_between(i32 %a, i32 %b, i32 %c, i32 %d, i64 %x, i64 %y) nounwind {
; CHECK-LABEL: name: wbool_between
; CHECK:       {{^  }}bb.0 (
; CHECK:         [[Z:%[0-9]+]]:gpr = CMPULTRW
; CHECK-NEXT:    CMPRRW_I {{%[0-9]+}}, {{%[0-9]+}}, implicit-def $flags
; CHECK-NEXT:    JMPLT %bb.2, implicit $flags
; CHECK-NOT:     CMPRRW_I
; CHECK:       {{^  }}bb.2 (
; CHECK-NEXT:    [[S1:%[0-9]+]]:gpr = PHI
; CHECK-NEXT:    [[S2:%[0-9]+]]:gpr = PHI
; CHECK-NEXT:    ADDR killed [[S1]], killed [[Z]]
  %cc = icmp slt i32 %a, %b
  %s1 = select i1 %cc, i64 %x, i64 %y
  %c2 = icmp ult i32 %c, %d
  %z2 = zext i1 %c2 to i64
  %s2 = select i1 %cc, i64 %y, i64 %x
  %r1 = add i64 %s1, %z2
  %r = mul i64 %r1, %s2
  ret i64 %r
}
