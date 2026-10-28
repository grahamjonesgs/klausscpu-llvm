; RUN: rm -rf %t && mkdir -p %t/g %t/n
; RUN: llc -mtriple=klausscpu -filetype=obj %s -o %t/g/x.o
; RUN: opt -passes=strip -S %s | llc -mtriple=klausscpu -filetype=obj -o %t/n/x.o
; RUN: cd %t/g && llvm-objdump -d --no-show-raw-insn x.o > %t/g.s
; RUN: cd %t/n && llvm-objdump -d --no-show-raw-insn x.o > %t/n.s
; RUN: diff %t/g.s %t/n.s
; RUN: llvm-objdump -d --no-show-raw-insn %t/g/x.o | FileCheck %s
; RUN: llvm-dwarfdump --debug-frame %t/g/x.o | FileCheck %s --check-prefix=FRAME
;
; The MachineOutliner (minsize) with debug info: -g changes no code (CFI
; instructions and the FrameSetup spills they follow are never outlined),
; and an outlined epilogue, which restores R4 and R5 and returns with
; LEAVERET from inside the frame of e1/e2/e3, has CFI that describes that
; frame, so a backtrace from it is right.  The other outlined functions
; run with the CIE's CFA = SP + 8.
;
; Generated from (clang -Oz -g):
;   extern long g(long);
;   long e1(long a, long b) { long x = g(a); long y = g(b + x); return g(y) * x + y; }
;   long e2(long a, long b) { long x = g(b); long y = g(a + x); return g(y) * x + y; }
;   long e3(long a, long b) { long x = g(a - 1); long y = g(b + x); return g(y) * x + y; }

; CHECK:      <e1>:
; CHECK:        jmprel {{.*}} <[[EPI:OUTLINED_FUNCTION_[0-9]+]]>
; CHECK:      <e2>:
; CHECK:        jmprel {{.*}} <[[EPI]]>
; CHECK:      <e3>:
; CHECK:        jmprel {{.*}} <[[EPI]]>
; CHECK:      <[[EPI]]>:
; CHECK:        ldidx64 r5, r15, -16
; CHECK:        ldidx64 r4, r15, -8
; CHECK-NEXT:   leaveret

; FRAME:      FDE cie=00000000 pc=00000068...0000007c
; FRAME-NEXT:   Format: DWARF32
; FRAME-NEXT:   DW_CFA_def_cfa: R15 +16
; FRAME-NEXT:   DW_CFA_offset: R15 -16
; FRAME-NEXT:   DW_CFA_offset: R4 -24
; FRAME-NEXT:   DW_CFA_offset: R5 -32
; FRAME:        0x68: CFA=R15+16: R4=[CFA-24], R5=[CFA-32], R15=[CFA-16], RA=[CFA-8]
; FRAME:      FDE cie=00000000 pc=0000007c...00000088
; FRAME-NEXT:   Format: DWARF32
; FRAME-EMPTY:
; FRAME-NEXT:   0x7c: CFA=SP+8: RA=[CFA-8]

target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64"
target triple = "klausscpu-unknown-unknown-elf"

; Function Attrs: minsize nounwind optsize
define dso_local i64 @e1(i64 noundef %a, i64 noundef %b) local_unnamed_addr #0 !dbg !10 {
entry:
    #dbg_value(i64 %a, !15, !DIExpression(), !19)
    #dbg_value(i64 %b, !16, !DIExpression(), !19)
  %call = tail call i64 @g(i64 noundef %a) #2, !dbg !20
    #dbg_value(i64 %call, !17, !DIExpression(), !19)
  %add = add nsw i64 %call, %b, !dbg !21
  %call1 = tail call i64 @g(i64 noundef %add) #2, !dbg !22
    #dbg_value(i64 %call1, !18, !DIExpression(), !19)
  %call2 = tail call i64 @g(i64 noundef %call1) #2, !dbg !23
  %mul = mul nsw i64 %call2, %call, !dbg !24
  %add3 = add nsw i64 %mul, %call1, !dbg !25
  ret i64 %add3, !dbg !26
}

; Function Attrs: minsize optsize
declare !dbg !27 dso_local i64 @g(i64 noundef) local_unnamed_addr #1

; Function Attrs: minsize nounwind optsize
define dso_local i64 @e2(i64 noundef %a, i64 noundef %b) local_unnamed_addr #0 !dbg !30 {
entry:
    #dbg_value(i64 %a, !32, !DIExpression(), !36)
    #dbg_value(i64 %b, !33, !DIExpression(), !36)
  %call = tail call i64 @g(i64 noundef %b) #2, !dbg !37
    #dbg_value(i64 %call, !34, !DIExpression(), !36)
  %add = add nsw i64 %call, %a, !dbg !38
  %call1 = tail call i64 @g(i64 noundef %add) #2, !dbg !39
    #dbg_value(i64 %call1, !35, !DIExpression(), !36)
  %call2 = tail call i64 @g(i64 noundef %call1) #2, !dbg !40
  %mul = mul nsw i64 %call2, %call, !dbg !41
  %add3 = add nsw i64 %mul, %call1, !dbg !42
  ret i64 %add3, !dbg !43
}

; Function Attrs: minsize nounwind optsize
define dso_local i64 @e3(i64 noundef %a, i64 noundef %b) local_unnamed_addr #0 !dbg !44 {
entry:
    #dbg_value(i64 %a, !46, !DIExpression(), !50)
    #dbg_value(i64 %b, !47, !DIExpression(), !50)
  %sub = add nsw i64 %a, -1, !dbg !51
  %call = tail call i64 @g(i64 noundef %sub) #2, !dbg !52
    #dbg_value(i64 %call, !48, !DIExpression(), !50)
  %add = add nsw i64 %call, %b, !dbg !53
  %call1 = tail call i64 @g(i64 noundef %add) #2, !dbg !54
    #dbg_value(i64 %call1, !49, !DIExpression(), !50)
  %call2 = tail call i64 @g(i64 noundef %call1) #2, !dbg !55
  %mul = mul nsw i64 %call2, %call, !dbg !56
  %add3 = add nsw i64 %mul, %call1, !dbg !57
  ret i64 %add3, !dbg !58
}

attributes #0 = { minsize nounwind optsize "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" }
attributes #1 = { minsize optsize "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" }
attributes #2 = { minsize nounwind optsize }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3, !4, !5}
!llvm.errno.tbaa = !{!6}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug, splitDebugInlining: false, nameTableKind: None)
!1 = !DIFile(filename: "outliner.c", directory: "/", checksumkind: CSK_MD5, checksum: "e1e8c785db1726f195839a9402a29fd1")
!2 = !{i32 7, !"Dwarf Version", i32 5}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !{i32 7, !"frame-pointer", i32 1}
!5 = !{i32 7, !"debug-info-assignment-tracking", i1 true}
!6 = !{!7, !7, i64 0}
!7 = !{!"int", !8, i64 0}
!8 = !{!"omnipotent char", !9, i64 0}
!9 = !{!"Simple C/C++ TBAA"}
!10 = distinct !DISubprogram(name: "e1", scope: !1, file: !1, line: 2, type: !11, scopeLine: 2, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !14, keyInstructions: true)
!11 = !DISubroutineType(types: !12)
!12 = !{!13, !13, !13}
!13 = !DIBasicType(name: "long", size: 64, encoding: DW_ATE_signed)
!14 = !{!15, !16, !17, !18}
!15 = !DILocalVariable(name: "a", arg: 1, scope: !10, file: !1, line: 2, type: !13)
!16 = !DILocalVariable(name: "b", arg: 2, scope: !10, file: !1, line: 2, type: !13)
!17 = !DILocalVariable(name: "x", scope: !10, file: !1, line: 2, type: !13)
!18 = !DILocalVariable(name: "y", scope: !10, file: !1, line: 2, type: !13)
!19 = !DILocation(line: 0, scope: !10)
!20 = !DILocation(line: 2, column: 36, scope: !10, atomGroup: 1, atomRank: 2)
!21 = !DILocation(line: 2, column: 55, scope: !10)
!22 = !DILocation(line: 2, column: 51, scope: !10, atomGroup: 2, atomRank: 2)
!23 = !DILocation(line: 2, column: 68, scope: !10)
!24 = !DILocation(line: 2, column: 73, scope: !10)
!25 = !DILocation(line: 2, column: 77, scope: !10, atomGroup: 4, atomRank: 2)
!26 = !DILocation(line: 2, column: 61, scope: !10, atomGroup: 4, atomRank: 1)
!27 = !DISubprogram(name: "g", scope: !1, file: !1, line: 1, type: !28, flags: DIFlagPrototyped, spFlags: DISPFlagOptimized)
!28 = !DISubroutineType(types: !29)
!29 = !{!13, !13}
!30 = distinct !DISubprogram(name: "e2", scope: !1, file: !1, line: 3, type: !11, scopeLine: 3, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !31, keyInstructions: true)
!31 = !{!32, !33, !34, !35}
!32 = !DILocalVariable(name: "a", arg: 1, scope: !30, file: !1, line: 3, type: !13)
!33 = !DILocalVariable(name: "b", arg: 2, scope: !30, file: !1, line: 3, type: !13)
!34 = !DILocalVariable(name: "x", scope: !30, file: !1, line: 3, type: !13)
!35 = !DILocalVariable(name: "y", scope: !30, file: !1, line: 3, type: !13)
!36 = !DILocation(line: 0, scope: !30)
!37 = !DILocation(line: 3, column: 36, scope: !30, atomGroup: 1, atomRank: 2)
!38 = !DILocation(line: 3, column: 55, scope: !30)
!39 = !DILocation(line: 3, column: 51, scope: !30, atomGroup: 2, atomRank: 2)
!40 = !DILocation(line: 3, column: 68, scope: !30)
!41 = !DILocation(line: 3, column: 73, scope: !30)
!42 = !DILocation(line: 3, column: 77, scope: !30, atomGroup: 4, atomRank: 2)
!43 = !DILocation(line: 3, column: 61, scope: !30, atomGroup: 4, atomRank: 1)
!44 = distinct !DISubprogram(name: "e3", scope: !1, file: !1, line: 4, type: !11, scopeLine: 4, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !45, keyInstructions: true)
!45 = !{!46, !47, !48, !49}
!46 = !DILocalVariable(name: "a", arg: 1, scope: !44, file: !1, line: 4, type: !13)
!47 = !DILocalVariable(name: "b", arg: 2, scope: !44, file: !1, line: 4, type: !13)
!48 = !DILocalVariable(name: "x", scope: !44, file: !1, line: 4, type: !13)
!49 = !DILocalVariable(name: "y", scope: !44, file: !1, line: 4, type: !13)
!50 = !DILocation(line: 0, scope: !44)
!51 = !DILocation(line: 4, column: 40, scope: !44)
!52 = !DILocation(line: 4, column: 36, scope: !44, atomGroup: 1, atomRank: 2)
!53 = !DILocation(line: 4, column: 59, scope: !44)
!54 = !DILocation(line: 4, column: 55, scope: !44, atomGroup: 2, atomRank: 2)
!55 = !DILocation(line: 4, column: 72, scope: !44)
!56 = !DILocation(line: 4, column: 77, scope: !44)
!57 = !DILocation(line: 4, column: 81, scope: !44, atomGroup: 4, atomRank: 2)
!58 = !DILocation(line: 4, column: 65, scope: !44, atomGroup: 4, atomRank: 1)
