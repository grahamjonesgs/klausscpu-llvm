; BEXTR / BDEP take their registers from the operand list (rd, rs1 and, for
; BDEP, the source rs2), so inline asm can use any allocation: BDEP used to
; have no source operand and always deposited r0.
;
; RUN: llc -mtriple=klausscpu -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -mtriple=klausscpu -filetype=obj < %s | llvm-objdump -d - \
; RUN:   | FileCheck %s --check-prefix=OBJ

; rd = r12, base = r1, source = r2: word 0 0x93400C12.
define i64 @deposit(i64 %unused, i64 %base, i64 %src) nounwind {
; CHECK-LABEL: deposit:
; CHECK:         #APP
; CHECK-NEXT:    bdep r12, r1, r2, 2052
; CHECK:         #NO_APP
; OBJ-LABEL: <deposit>:
; OBJ:         12 0c 40 93 04 08 00 00    bdep r12, r1, r2, 2052
  %r = call i64 asm "bdep $0, $1, $2, 0x0804", "={r12},{r1},{r2}"(i64 %base, i64 %src)
  ret i64 %r
}

; rd = r12, rs1 = r3: word 0 0x93000C30.
define i64 @extract(i64 %a, i64 %b, i64 %c, i64 %x) nounwind {
; CHECK-LABEL: extract:
; CHECK:         #APP
; CHECK-NEXT:    bextr r12, r3, 2052
; CHECK:         #NO_APP
; OBJ-LABEL: <extract>:
; OBJ:         30 0c 00 93 04 08 00 00    bextr r12, r3, 2052
  %r = call i64 asm "bextr $0, $1, 0x0804", "={r12},{r3}"(i64 %x)
  ret i64 %r
}

; Register-allocated operands.
define i64 @deposit_any(i64 %base, i64 %src) nounwind {
; CHECK-LABEL: deposit_any:
; CHECK:         #APP
; CHECK-NEXT:    bdep {{r[0-9]+}}, r0, r1, 31
; CHECK:         #NO_APP
  %r = call i64 asm "bdep $0, $1, $2, 31", "=r,r,r"(i64 %base, i64 %src)
  ret i64 %r
}
