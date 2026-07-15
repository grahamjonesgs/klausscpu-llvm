; M8 scheduling model (KlaussCPUSchedule.td) — regression guards.
;
; The pre-RA MISched and the post-RA list scheduler both consume KlaussCPUSchedModel.
; PostRAScheduler is ON.  The hazard it must respect: a conditional branch reads the
; FLAGS a compare set, so no flag-clobbering op (e.g. a frame-address ADDI) may be
; scheduled into the compare->branch gap.  FLAGS is modeled (Defs on flag-setting
; ops, Uses on conditional branches), so the compare and its branch stay adjacent.
;
; RUN: llc -march=klausscpu -O2 < %s | FileCheck %s
;
; A/B baseline (spec §7.1): -mcpu=no-sched selects NoSchedModel — the whole M8
; transform off — and must still produce correct code.
; RUN: llc -march=klausscpu -mcpu=no-sched -O2 < %s | FileCheck %s --check-prefix=NOSCHED

declare void @sink(ptr)

; A relational guard (slt 5) keeps an explicit CMPRV (A3a flag-reuse only folds the
; ==0 / !=0 forms), and the escaping alloca forces a frame-address materialisation the
; post-RA scheduler could otherwise try to hoist between the compare and the branch.
; The compare must be immediately followed by its conditional branch.
define i64 @guard_not_split(i64 %n) {
; The compare (immediate is a canonicalisation detail: slt 5 -> cmprv,4 + jmpgt)
; must be immediately followed by its conditional branch — nothing hoisted between.
; CHECK-LABEL: guard_not_split:
; CHECK:      cmprv r{{[0-9]+}}, {{[0-9]+}}
; CHECK-NEXT: jmp{{[gl][te]}} .LBB
;
; NOSCHED-LABEL: guard_not_split:
; NOSCHED:    cmprv r{{[0-9]+}}, {{[0-9]+}}
entry:
  %buf = alloca [4 x i64]
  call void @sink(ptr %buf)
  %lt = icmp slt i64 %n, 5
  br i1 %lt, label %small, label %big
small:
  ret i64 %n
big:
  ret i64 0
}
