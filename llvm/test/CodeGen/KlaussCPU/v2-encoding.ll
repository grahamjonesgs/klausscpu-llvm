; RUN: llc -march=klausscpu -O2 -filetype=obj < %s \
; RUN:   | llvm-objdump -s -j .text - | FileCheck %s
;
; Byte-level regression test for the ISA v2 (flag-day) encoding.  Each CHECK is
; a distinctive little-endian instruction word from llvm-objdump's `-s` hex dump
; (bytes shown lowest-address-first).  These pin the v2 opcode *templates* and
; the register-field rules that differ from v1:
;   * uniform fields  rd[11:8] / rs1[7:4] / rs2[3:0]
;   * STORES carry their data source in the rd field
;   * shift/bit-by-immediate and register-offset indexed ops are single words

; RRR: addr r12, r0, r1  -> 0x44200C01  (rd=12, rs1=0, rs2=1)
define i64 @f_addr(i64 %a, i64 %b) {
  %r = add i64 %a, %b
  ret i64 %r
}
; CHECK-DAG: 010c2044

; Prologue/epilogue register fields:
;   push  r15 -> 0x640000F0   getsp r15 -> 0x64C00F00
;   setsp r15 -> 0x650000F0   pop   r15 -> 0x64800F00   ret -> 0x65800000
; CHECK-DAG: f0000064
; CHECK-DAG: 000fc064
; CHECK-DAG: f0000065
; CHECK-DAG: 000f8064
; CHECK-DAG: 00008065

; Store puts its data register in rd[11:8]:
;   stidx64 r1, r0, 0 -> word0 0x9F300100
define void @f_store(ptr %p, i64 %v) {
  store i64 %v, ptr %p
  ret void
}
; CHECK-DAG: 0001309f

; Truncating sub-word store, data in rd:
;   memset8 r1, r0 -> 0x5C000100
define void @f_store8(ptr %p, i64 %v) {
  %t = trunc i64 %v to i8
  store i8 %t, ptr %p
  ret void
}
; CHECK-DAG: 0001005c

; Shift-by-immediate is now a single word with the count embedded at bit 15:
;   shlv r0, 3 -> 0x5021C000
define i64 @f_shl(i64 %a) {
  %r = shl i64 %a, 3
  ret i64 %r
}
; CHECK-DAG: 00c02150

; 64-bit constant: setr64 r12, ... -> word0 0xCBC00C00 (LEN=11, 3 words)
define i64 @f_big() {
  ret i64 5000000000
}
; CHECK-DAG: 000cc0cb
