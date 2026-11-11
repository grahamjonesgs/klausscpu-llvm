; RUN: llc -mtriple=klausscpu-unknown-linux-musl -verify-machineinstrs -O2 < %s \
; RUN:   | FileCheck %s --check-prefix=LINUX
; RUN: llc -mtriple=klausscpu-unknown-linux-musl -relocation-model=pic \
; RUN:   -verify-machineinstrs -O2 < %s | FileCheck %s --check-prefix=LINUX
; RUN: llc -mtriple=klausscpu-unknown-elf -verify-machineinstrs -O2 < %s \
; RUN:   | FileCheck %s --check-prefix=BARE
; RUN: llc -mtriple=klausscpu-unknown-elf -relocation-model=pic \
; RUN:   -verify-machineinstrs -O2 < %s | FileCheck %s --check-prefix=BARE
; RUN: llc -mtriple=klausscpu-unknown-linux-musl -relocation-model=pic -O2 \
; RUN:   -filetype=obj < %s | llvm-readobj -r -s - \
; RUN:   | FileCheck %s --check-prefix=OBJ

; Exception tables on Linux (ABI.md "Dynamic linking"), PIC or not: the
; personality is indirect|pcrel|sdata4 (0x9b) through an 8-byte
; DW.ref.<personality>, the LSDA pcrel|sdata4 (0x1b), type info
; indirect|pcrel|sdata4 (0x9b), through an 8-byte stub.  Other triples keep
; absptr (which MC writes as 4 bytes).

@_ZTIi = external constant ptr

declare void @may_throw()
declare void @abort()
declare i32 @__gxx_personality_v0(...)

; The landing pad does not read the exception pointer or the selector: the
; backend does not define their registers (getExceptionPointerRegister) yet.
define i32 @catcher() personality ptr @__gxx_personality_v0 {
entry:
  invoke void @may_throw()
          to label %ok unwind label %lpad
ok:
  ret i32 0
lpad:
  %lp = landingpad { ptr, i32 }
          catch ptr @_ZTIi
  call void @abort()
  unreachable
}

; LINUX:        .cfi_personality 155, DW.ref.__gxx_personality_v0
; LINUX-NEXT:   .cfi_lsda 27, .Lexception0
; LINUX:      GCC_except_table0:
; LINUX:        .byte 255 # @LPStart Encoding = omit
; LINUX-NEXT:   .byte 155 # @TType Encoding = indirect pcrel sdata4
; LINUX:      .Ltmp3: # TypeInfo 1
; LINUX-NEXT:   .long .L_ZTIi.DW.stub-.Ltmp3
; LINUX:        .data
; LINUX-NEXT:   .p2align 3, 0x0
; LINUX-NEXT: .L_ZTIi.DW.stub:
; LINUX-NEXT:   .quad _ZTIi
; LINUX-NEXT:   .hidden DW.ref.__gxx_personality_v0
; LINUX-NEXT:   .weak DW.ref.__gxx_personality_v0
; LINUX-NEXT:   .section .data.DW.ref.__gxx_personality_v0,"awG",@progbits,DW.ref.__gxx_personality_v0,comdat
; LINUX-NEXT:   .p2align 3, 0x0
; LINUX-NEXT:   .type DW.ref.__gxx_personality_v0,@object
; LINUX-NEXT:   .size DW.ref.__gxx_personality_v0, 8
; LINUX-NEXT: DW.ref.__gxx_personality_v0:
; LINUX-NEXT:   .quad __gxx_personality_v0

; BARE:         .cfi_personality 0, __gxx_personality_v0
; BARE-NEXT:    .cfi_lsda 0, .Lexception0
; BARE:         .byte 0 # @TType Encoding = absptr
; BARE:         .long _ZTIi # TypeInfo 1
; BARE-NOT:     DW.ref

; OBJ:      .rela.gcc_except_table {
; OBJ-NEXT:   R_KLAUSSCPU_PC32 .data 0x0
; OBJ-NEXT: }
; OBJ:      .rela.data {
; OBJ-NEXT:   R_KLAUSSCPU_ABS64 _ZTIi 0x0
; OBJ-NEXT: }
; OBJ:      .rela.data.DW.ref.__gxx_personality_v0 {
; OBJ-NEXT:   R_KLAUSSCPU_ABS64 __gxx_personality_v0 0x0
; OBJ-NEXT: }
; OBJ:      .rela.eh_frame {
; OBJ-NEXT:   R_KLAUSSCPU_PC32 DW.ref.__gxx_personality_v0 0x0
; OBJ-NEXT:   R_KLAUSSCPU_PC32 .text 0x0
; OBJ-NEXT:   R_KLAUSSCPU_PC32 .gcc_except_table 0x0
; OBJ-NEXT: }
; OBJ:      Name: DW.ref.__gxx_personality_v0
; OBJ-NEXT: Value: 0x0
; OBJ-NEXT: Size: 8
