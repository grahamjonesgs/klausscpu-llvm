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

; itoa-class flag hazard (picolibc __ultoa_invert): a pointer increment (INCR — a
; flag CLOBBERER, so R_inplace carries Defs=[FLAGS]) inside a loop whose condition
; also sets flags.  With scheduling on, INCR must NOT be hoisted into the
; compare->branch gap; if it were, the branch would read INCR's flags (pointer != 0,
; always true) and spin forever — the board printf/itoa hang.
;
; A *runtime* base forces DIVUR (not constant-fold to MULHUR); the latency of DIVUR
; is exactly what tempts the scheduler to fill the gap with the independent INCR.
; This only schedules correctly because FLAGS is a TRACKED (non-reserved) physreg —
; reserving it hides the live range and the scheduler slots INCR into the gap.
; Pin: the compare is immediately followed by its branch (nothing flag-setting
; between), i.e. INCR is scheduled before the compare.
define ptr @itoa_no_flag_clobber(i64 %n, ptr %p, i64 %base) {
; The flag-clobbering INCR must not appear between the compare and its branch
; (a flag-neutral COPY there is fine — COPY_R does not set FLAGS).
; CHECK-LABEL: itoa_no_flag_clobber:
; CHECK:         cmprr r{{[0-9]+}}, r{{[0-9]+}}
; CHECK-NOT:     incr
; CHECK:         jmp{{[a-z]+}} .LBB
;
; NOSCHED-LABEL: itoa_no_flag_clobber:
; NOSCHED:       cmprr r{{[0-9]+}}, r{{[0-9]+}}
; NOSCHED-NOT:   incr
; NOSCHED:       jmp{{[a-z]+}} .LBB
entry:
  br label %loop
loop:
  %nv = phi i64 [ %n, %entry ], [ %ndiv, %loop ]
  %pv = phi ptr [ %p, %entry ], [ %pn, %loop ]
  %ndiv = udiv i64 %nv, %base
  %mul = mul i64 %ndiv, %base
  %rem = sub i64 %nv, %mul
  %digit = trunc i64 %rem to i8
  %c = add i8 %digit, 48
  store i8 %c, ptr %pv
  %pn = getelementptr i8, ptr %pv, i64 1
  %cond = icmp uge i64 %ndiv, %base
  br i1 %cond, label %loop, label %done
done:
  ret ptr %pn
}
