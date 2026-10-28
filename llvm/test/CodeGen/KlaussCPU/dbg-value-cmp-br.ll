; Debug info must not change codegen. With -g, LiveDebugVariables can leave a
; DBG_VALUE between the compare and the conditional branch; FuseCmpBr and
; FlagReuse skip debug instructions when pairing them, so each function below
; gets the same code with and without debug info. The last two RUN lines turn
; fusion off so that @count_down exercises FlagReuse on its own.
;
; RUN: llc -march=klausscpu -verify-machineinstrs -O1 < %s \
; RUN:   | FileCheck %s --check-prefixes=CHECK,FUSE
; RUN: opt -strip-debug < %s | llc -march=klausscpu -verify-machineinstrs -O1 \
; RUN:   | FileCheck %s --check-prefixes=CHECK,FUSE
; RUN: llc -march=klausscpu -verify-machineinstrs -O1 -klausscpu-fuse-cmp-br=false \
; RUN:   < %s | FileCheck %s
; RUN: opt -strip-debug < %s | llc -march=klausscpu -verify-machineinstrs -O1 \
; RUN:   -klausscpu-fuse-cmp-br=false | FileCheck %s

target triple = "klausscpu-unknown-elf"

define void @spin(i64 %k, i1 %c) !dbg !4 {
; CHECK-LABEL: spin:
; FUSE:          andv r12, r1, 1
; FUSE:        .LBB0_1:
; FUSE-NOT:      cmprv
; FUSE:          fbri r12, 0, 0, .LBB0_1
; FUSE-NEXT:   # %bb.2:
; FUSE-NEXT:     ret
entry:
  br label %loop

loop:
    #dbg_value(i64 %k, !7, !DIExpression(), !9)
  br i1 %c, label %exit, label %loop

exit:
  ret void
}

; FlagReuse: the DBG_VALUE lands between `CMPRV r1, 0` and the JMPNE; the
; compare must still go, with the branch reading DECR's Z.
define void @count_down(i64 %k, i64 %n0, ptr %p) !dbg !10 {
; CHECK-LABEL: count_down:
; CHECK:       .LBB1_1:
; CHECK:         stidx64 r1, r2, 0
; CHECK-NEXT:    decr r1, r1
; CHECK-NOT:     cmprv
; CHECK:         jmpnzrel .LBB1_1
; CHECK-NEXT:  # %bb.2:
; CHECK-NEXT:    ret
entry:
  br label %loop

loop:
  %i = phi i64 [ %n0, %entry ], [ %n, %loop ]
  store volatile i64 %i, ptr %p
  %n = add i64 %i, -1
  %c = icmp eq i64 %n, 0
    #dbg_value(i64 %k, !11, !DIExpression(), !12)
  br i1 %c, label %exit, label %loop

exit:
  ret void
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!3}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "t.c", directory: "/")
!2 = !{}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = distinct !DISubprogram(name: "spin", scope: !1, file: !1, line: 1, type: !6, scopeLine: 1, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !2)
!6 = !DISubroutineType(types: !2)
!7 = !DILocalVariable(name: "k", arg: 1, scope: !4, file: !1, line: 1, type: !8)
!8 = !DIBasicType(name: "long", size: 64, encoding: DW_ATE_signed)
!9 = !DILocation(line: 0, scope: !4)
!10 = distinct !DISubprogram(name: "count_down", scope: !1, file: !1, line: 5, type: !6, scopeLine: 5, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !2)
!11 = !DILocalVariable(name: "k", arg: 1, scope: !10, file: !1, line: 5, type: !8)
!12 = !DILocation(line: 0, scope: !10)
