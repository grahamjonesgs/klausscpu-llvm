; RUN: llc -mtriple=klausscpu -verify-machineinstrs -O1 -show-mc-encoding < %s \
; RUN:   | FileCheck %s --check-prefixes=CHECK,MULV
; RUN: llc -mtriple=klausscpu -verify-machineinstrs -O1 -show-mc-encoding \
; RUN:   -klausscpu-mul-imm-shift-add < %s | FileCheck %s --check-prefixes=CHECK,SHADD
; RUN: llc -mtriple=klausscpu -verify-machineinstrs -O1 -show-mc-encoding \
; RUN:   -klausscpu-mul-imm-shift-add -mcpu=no-sched < %s \
; RUN:   | FileCheck %s --check-prefixes=CHECK,MULV
;
; Immediates the 1-word short forms reach (imm8 [19:12]; the CPU sign- or
; zero-extends it per SGN): an add of 128..255 is the zero-extending short
; ADDV, an add of -255..-129 (a value or a frame address) the short MINUSV of
; the negation; ADDI's own short form covers -128..127.
;
; Multiply by a constant: MULV takes a simm32.  A constant 2^N + 1, 2^N - 1
; or 1 - 2^N that is not a simm32 (SETRU/SETR64 + MULR) becomes a shift and
; an add/sub.  For a simm32 one that trades one 2-word MULV for two 1-word
; ALU ops, only with -klausscpu-mul-imm-shift-add and a schedule model that
; rates the multiply slower (not -mcpu=no-sched).

define i64 @add_200(i64 %x) nounwind {
; CHECK-LABEL: add_200:
; CHECK:       addv r12, r0, 200 # encoding: [0x00,0x8c,0x2c,0x48]
  %r = add i64 %x, 200
  ret i64 %r
}

define i64 @add_255(i64 %x) nounwind {
; CHECK-LABEL: add_255:
; CHECK:       addv r12, r0, 255 # encoding: [0x00,0xfc,0x2f,0x48]
  %r = add i64 %x, 255
  ret i64 %r
}

define i64 @add_256(i64 %x) nounwind {
; CHECK-LABEL: add_256:
; CHECK:       addi r12, r0, 256 # encoding: [0x00,0x0c,0x30,0x88,0x00,0x01,0x00,0x00]
  %r = add i64 %x, 256
  ret i64 %r
}

define i64 @sub_128(i64 %x) nounwind {
; CHECK-LABEL: sub_128:
; CHECK:       addi r12, r0, -128 # encoding: [0x00,0x0c,0x38,0x48]
  %r = sub i64 %x, 128
  ret i64 %r
}

define i64 @sub_129(i64 %x) nounwind {
; CHECK-LABEL: sub_129:
; CHECK:       minusv r12, r0, 129 # encoding: [0x00,0x1c,0x68,0x48]
  %r = sub i64 %x, 129
  ret i64 %r
}

define i64 @add_m200(i64 %x) nounwind {
; CHECK-LABEL: add_m200:
; CHECK:       minusv r12, r0, 200 # encoding: [0x00,0x8c,0x6c,0x48]
  %r = add i64 %x, -200
  ret i64 %r
}

define i64 @sub_255(i64 %x) nounwind {
; CHECK-LABEL: sub_255:
; CHECK:       minusv r12, r0, 255 # encoding: [0x00,0xfc,0x6f,0x48]
  %r = sub i64 %x, 255
  ret i64 %r
}

define i64 @sub_256(i64 %x) nounwind {
; CHECK-LABEL: sub_256:
; CHECK:       addi r12, r0, -256 # encoding: [0x00,0x0c,0x30,0x88,0x00,0xff,0xff,0xff]
  %r = sub i64 %x, 256
  ret i64 %r
}

; The address of a frame slot 200 bytes below R15.
declare void @use(ptr)
define void @frame_addr() nounwind {
; CHECK-LABEL: frame_addr:
; CHECK:       minusv r0, r15, 200 # encoding: [0xf0,0x80,0x6c,0x48]
; CHECK:       callrel use
  %buf = alloca [200 x i8], align 8
  call void @use(ptr %buf)
  ret void
}

define i64 @mul_7(i64 %x) nounwind {
; CHECK-LABEL: mul_7:
; MULV:        mulv r12, r0, 7
; SHADD:       shlv r12, r0, 3
; SHADD-NEXT:  subr r12, r12, r0
  %r = mul i64 %x, 7
  ret i64 %r
}

define i64 @mul_9(i64 %x) nounwind {
; CHECK-LABEL: mul_9:
; MULV:        mulv r12, r0, 9
; SHADD:       shlv r12, r0, 3
; SHADD-NEXT:  addr r12, r12, r0
  %r = mul i64 %x, 9
  ret i64 %r
}

define i64 @mul_m7(i64 %x) nounwind {
; CHECK-LABEL: mul_m7:
; MULV:        mulv r12, r0, -7
; SHADD:       shlv r12, r0, 3
; SHADD-NEXT:  subr r12, r0, r12
  %r = mul i64 %x, -7
  ret i64 %r
}

define i32 @mul_i32_5(i32 %x) nounwind {
; CHECK-LABEL: mul_i32_5:
; MULV:        mulv r12, r0, 5
; SHADD:       shlv r12, r0, 2
; SHADD-NEXT:  addr r12, r12, r0
  %r = mul i32 %x, 5
  ret i32 %r
}

; -3 = -(2^1 + 1) would need a NEGR, 10 = 5 * 2 a second shift.
define i64 @mul_m3(i64 %x) nounwind {
; CHECK-LABEL: mul_m3:
; CHECK:       mulv r12, r0, -3
  %r = mul i64 %x, -3
  ret i64 %r
}

define i64 @mul_10(i64 %x) nounwind {
; CHECK-LABEL: mul_10:
; CHECK:       mulv r12, r0, 10
  %r = mul i64 %x, 10
  ret i64 %r
}

define i64 @mul_2p32_plus_1(i64 %x) nounwind {
; CHECK-LABEL: mul_2p32_plus_1:
; CHECK:       shlv r12, r0, 32
; CHECK-NEXT:  addr r12, r12, r0
  %r = mul i64 %x, 4294967297
  ret i64 %r
}

define i64 @mul_2p32_minus_1(i64 %x) nounwind {
; CHECK-LABEL: mul_2p32_minus_1:
; CHECK:       shlv r12, r0, 32
; CHECK-NEXT:  subr r12, r12, r0
  %r = mul i64 %x, 4294967295
  ret i64 %r
}

define i64 @mul_1_minus_2p40(i64 %x) nounwind {
; CHECK-LABEL: mul_1_minus_2p40:
; CHECK:       shlv r12, r0, 40
; CHECK-NEXT:  subr r12, r0, r12
  %r = mul i64 %x, -1099511627775
  ret i64 %r
}

define i64 @mul_m_2p32_plus_1(i64 %x) nounwind {
; CHECK-LABEL: mul_m_2p32_plus_1:
; CHECK:       setr64 r12, -1, -2
; CHECK-NEXT:  mulr r12, r0, r12
  %r = mul i64 %x, -4294967297
  ret i64 %r
}

; (mul (add x, c1), c2) is not distributed when c1 is an add immediate and
; c1 * c2 is not.
define i64 @mul_add(i64 %x) nounwind {
; CHECK-LABEL: mul_add:
; MULV:        addi r12, r0, 2147483647
; MULV-NEXT:   mulv r12, r12, 3
  %a = add i64 %x, 2147483647
  %r = mul i64 %a, 3
  ret i64 %r
}

define i64 @mul_add_small(i64 %x) nounwind {
; CHECK-LABEL: mul_add_small:
; MULV:        mulv r12, r0, 3
; MULV-NEXT:   addi r12, r12, 15
  %a = add i64 %x, 5
  %r = mul i64 %a, 3
  ret i64 %r
}
