; RUN: llc -mtriple=klausscpu-unknown-linux-musl -filetype=obj < %s -o %t.o
; RUN: llvm-readobj -r %t.o | FileCheck %s

; The "CG Profile" module flag (clang -fprofile-use, the CGProfile pass) is
; emitted as .cg_profile, whose R_KLAUSSCPU_NONE relocations need
; BFD_RELOC_NONE: a PGO build of anything failed with "unknown relocation
; name".

; @hot is internal, the usual PGO case: its relocation must name its own
; (local) symbol, not the section's, for lld to find it.
define internal void @hot() {
  ret void
}

define void @caller() {
  call void @hot()
  call void @hot()
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 5, !"CG Profile", !1}
!1 = !{!2}
!2 = !{ptr @caller, ptr @hot, i64 1000}

; CHECK:      Section ({{.*}}) .rel.llvm.call-graph-profile {
; CHECK-NEXT:   0x0 R_KLAUSSCPU_NONE caller{{$}}
; CHECK-NEXT:   0x0 R_KLAUSSCPU_NONE hot{{$}}
; CHECK-NEXT: }
