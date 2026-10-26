; Debug info must not change codegen. With -g, LiveDebugVariables can leave a
; DBG_VALUE between the compare and the conditional branch; FuseCmpBr and
; FlagReuse skip debug instructions when pairing them, so the loop below gets
; the same fused branch with and without debug info.
;
; RUN: llc -march=klausscpu -verify-machineinstrs -O1 < %s | FileCheck %s
; RUN: opt -strip-debug < %s | llc -march=klausscpu -verify-machineinstrs -O1 \
; RUN:   | FileCheck %s

target triple = "klausscpu-unknown-elf"

define void @spin(i64 %k, i1 %c) !dbg !4 {
; CHECK-LABEL: spin:
; CHECK:         andv r12, r1, 1
; CHECK:       .LBB0_1:
; CHECK-NOT:     cmprv
; CHECK:         fbri r12, 0, 0, .LBB0_1
; CHECK-NEXT:  # %bb.2:
; CHECK-NEXT:    leaveret
entry:
  br label %loop

loop:
    #dbg_value(i64 %k, !7, !DIExpression(), !9)
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
