; RUN: llc -march=klausscpu -O1 < %s | FileCheck %s

; Bit-manipulation ops Rust's core leans on: rotates, abs, popcount,
; leading/trailing zeros, byte/bit reverse, high multiply.
; All must select the hardware instruction, never a multi-op expansion.
; (POPCNT/CLZ/CTZ/BITREV/BSWAP/ABSR are in-place R-format ops: the result
; lands in the input register and is then copied to R12 for the return.)

define i64 @rotl64(i64 %a, i64 %n) {
; CHECK-LABEL: rotl64:
; CHECK: rolr r12, r0, r1
; CHECK-NOT: shlr
; CHECK-NOT: shrr
  %r = call i64 @llvm.fshl.i64(i64 %a, i64 %a, i64 %n)
  ret i64 %r
}

define i64 @rotr64(i64 %a, i64 %n) {
; CHECK-LABEL: rotr64:
; CHECK: rorr r12, r0, r1
; CHECK-NOT: shlr
; CHECK-NOT: shrr
  %r = call i64 @llvm.fshr.i64(i64 %a, i64 %a, i64 %n)
  ret i64 %r
}

define i64 @abs64(i64 %a) {
; CHECK-LABEL: abs64:
; CHECK: absr r{{[0-9]+}}
; CHECK-NOT: sarr
  %r = call i64 @llvm.abs.i64(i64 %a, i1 false)
  ret i64 %r
}

define i64 @popcount64(i64 %a) {
; CHECK-LABEL: popcount64:
; CHECK: popcnt r{{[0-9]+}}
  %r = call i64 @llvm.ctpop.i64(i64 %a)
  ret i64 %r
}

; Hardware CLZ(0)/CTZ(0) both return 64 (confirmed 2026-06-12), matching
; LLVM's zero-defined semantics — so even the zero-defined forms (what
; u64::leading_zeros emits) must be a bare CLZ/CTZ with no zero guard.
define i64 @leading_zeros64(i64 %a) {
; CHECK-LABEL: leading_zeros64:
; CHECK-NOT: cmprv
; CHECK: clz r{{[0-9]+}}
; CHECK-NOT: jmpe
; CHECK-NOT: shrr
  %r = call i64 @llvm.ctlz.i64(i64 %a, i1 false)
  ret i64 %r
}

; Zero-undef variant (NonZeroU64::leading_zeros) must be a bare CLZ.
define i64 @leading_zeros_nonzero(i64 %a) {
; CHECK-LABEL: leading_zeros_nonzero:
; CHECK-NOT: cmprv
; CHECK: clz r{{[0-9]+}}
; CHECK-NOT: jmpe
  %r = call i64 @llvm.ctlz.i64(i64 %a, i1 true)
  ret i64 %r
}

define i64 @trailing_zeros64(i64 %a) {
; CHECK-LABEL: trailing_zeros64:
; CHECK-NOT: cmprv
; CHECK: ctz r{{[0-9]+}}
; CHECK-NOT: jmpe
; CHECK-NOT: shlr
  %r = call i64 @llvm.cttz.i64(i64 %a, i1 false)
  ret i64 %r
}

define i64 @trailing_zeros_nonzero(i64 %a) {
; CHECK-LABEL: trailing_zeros_nonzero:
; CHECK-NOT: cmprv
; CHECK: ctz r{{[0-9]+}}
; CHECK-NOT: jmpe
  %r = call i64 @llvm.cttz.i64(i64 %a, i1 true)
  ret i64 %r
}

define i64 @bswap64(i64 %a) {
; CHECK-LABEL: bswap64:
; CHECK: bswap r{{[0-9]+}}
; CHECK-NOT: shrr
  %r = call i64 @llvm.bswap.i64(i64 %a)
  ret i64 %r
}

define i64 @bitrev64(i64 %a) {
; CHECK-LABEL: bitrev64:
; CHECK: bitrev r{{[0-9]+}}
; CHECK-NOT: shrr
  %r = call i64 @llvm.bitreverse.i64(i64 %a)
  ret i64 %r
}

define i64 @mulh_s(i64 %a, i64 %b) {
; CHECK-LABEL: mulh_s:
; CHECK: mulhr r12, r0, r1
  %ae = sext i64 %a to i128
  %be = sext i64 %b to i128
  %m = mul i128 %ae, %be
  %h = lshr i128 %m, 64
  %r = trunc i128 %h to i64
  ret i64 %r
}

define i64 @mulh_u(i64 %a, i64 %b) {
; CHECK-LABEL: mulh_u:
; CHECK: mulhur r12, r0, r1
  %ae = zext i64 %a to i128
  %be = zext i64 %b to i128
  %m = mul i128 %ae, %be
  %h = lshr i128 %m, 64
  %r = trunc i128 %h to i64
  ret i64 %r
}

; i32 rotate must NOT use the 64-bit ROLR (a 64-bit register rotate is not a
; 32-bit rotate) — shift-pair expansion expected.
define i32 @rotl32(i32 %a, i32 %n) {
; CHECK-LABEL: rotl32:
; CHECK-NOT: rolr
  %r = call i32 @llvm.fshl.i32(i32 %a, i32 %a, i32 %n)
  ret i32 %r
}

declare i64 @llvm.fshl.i64(i64, i64, i64)
declare i64 @llvm.fshr.i64(i64, i64, i64)
declare i32 @llvm.fshl.i32(i32, i32, i32)
declare i64 @llvm.abs.i64(i64, i1)
declare i64 @llvm.ctpop.i64(i64)
declare i64 @llvm.ctlz.i64(i64, i1)
declare i64 @llvm.cttz.i64(i64, i1)
declare i64 @llvm.bswap.i64(i64)
declare i64 @llvm.bitreverse.i64(i64)
