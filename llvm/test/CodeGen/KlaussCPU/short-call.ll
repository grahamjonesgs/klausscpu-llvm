; RUN: llc -mtriple=klausscpu -O1 -verify-machineinstrs -relocation-model=pic \
; RUN:   -filetype=obj < %s | llvm-objdump -dr - | FileCheck %s
; RUN: llc -mtriple=klausscpu -O1 -verify-machineinstrs -filetype=obj < %s \
; RUN:   | llvm-objdump -dr - | FileCheck %s
; RUN: llc -mtriple=klausscpu -O1 -verify-machineinstrs -relocation-model=pic \
; RUN:   -klausscpu-short-calls=false -filetype=obj < %s \
; RUN:   | llvm-objdump -dr --no-show-raw-insn - | FileCheck %s --check-prefix=PIC-LONG
; RUN: llc -mtriple=klausscpu -O1 -verify-machineinstrs \
; RUN:   -klausscpu-short-calls=false -filetype=obj < %s \
; RUN:   | llvm-objdump -dr --no-show-raw-insn - | FileCheck %s --check-prefix=STATIC-LONG
; RUN: llc -mtriple=klausscpu -O1 -verify-machineinstrs < %s \
; RUN:   | FileCheck %s --check-prefix=ASM

; Every direct call, PIC or static, to a function or a libcall, is the 1-word
; short call (ISA v3 A1, LINK=1, word 0 0x63000000 | simm18): resolved by the
; assembler in the same section, else an R_KLAUSSCPU_CALL18 relocation that
; lld resolves (through a thunk when the callee is out of reach).  With
; -klausscpu-short-calls=false calls are the 2-word CALLREL (PIC) / CALL.

declare i64 @ext(i64)

define internal i64 @local(i64 %x) noinline {
  %r = mul i64 %x, %x
  ret i64 %r
}

; CHECK-LABEL: <calls>:
; CHECK:       00 00 00 63 callrel
; CHECK-NEXT:  R_KLAUSSCPU_CALL18 ext
; CHECK:       {{[0-9a-f]{2} [0-9a-f]{2} 0[0-3] 63}} callrel {{.*}} <local>
; CHECK-NOT:   R_KLAUSSCPU
; CHECK:       00 00 00 63 callrel
; CHECK-NEXT:  R_KLAUSSCPU_CALL18 __divti3
; PIC-LONG-LABEL: <calls>:
; PIC-LONG:       callrel
; PIC-LONG-NEXT:  R_KLAUSSCPU_PCREL32 ext
; PIC-LONG:       callrel {{.*}} <local>
; PIC-LONG:       callrel
; PIC-LONG-NEXT:  R_KLAUSSCPU_PCREL32 __divti3
; STATIC-LONG-LABEL: <calls>:
; STATIC-LONG:       call 0x0
; STATIC-LONG-NEXT:  R_KLAUSSCPU_ABS32 ext
; STATIC-LONG:       call 0x0
; STATIC-LONG-NEXT:  R_KLAUSSCPU_ABS32 .text
; STATIC-LONG:       call 0x0
; STATIC-LONG-NEXT:  R_KLAUSSCPU_ABS32 __divti3
; ASM-LABEL: calls:
; ASM:         callrel ext
; ASM:         callrel local
; ASM:         callrel __divti3
define i128 @calls(i64 %a, i128 %n, i128 %d) {
  %x = call i64 @ext(i64 %a)
  %y = call i64 @local(i64 %x)
  %q = sdiv i128 %n, %d
  %z = zext i64 %y to i128
  %r = add i128 %q, %z
  ret i128 %r
}
