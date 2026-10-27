; RUN: llc -O1 -filetype=obj < %s -o %t.o
; RUN: llvm-readobj --file-headers %t.o | FileCheck %s --check-prefix=HDR
; RUN: llvm-dwarfdump --debug-frame %t.o | FileCheck %s --check-prefix=FRAME
; RUN: llvm-dwarfdump --debug-line %t.o | FileCheck %s --check-prefix=LINE
; RUN: llvm-dwarfdump --verify %t.o | FileCheck %s --check-prefix=VERIFY

; Object-level debug info: the ELF machine maps back to the KlaussCPU target,
; R_KLAUSSCPU_ABS32 relocations in the debug sections are resolved (FDE and
; line-table addresses of the second function are non-zero), and the CFI
; decodes into the expected unwind rows.
;
; Generated from:
;   long ext(long);
;   long first(long a) { return ext(a) + 1; }
;   long second(long a, long b) {
;     long x = ext(a), y = ext(b);
;     return ext(x * y) + x + y;
;   }

; HDR:      Format: elf32-klausscpu
; HDR-NEXT: Arch: klausscpu
; HDR:      Machine: EM_KLAUSSCPU (0x4B43)

; FRAME:      CIE
; FRAME:      Return address column: 17
; FRAME:      DW_CFA_def_cfa: SP +8
; FRAME-NEXT: DW_CFA_offset: RA -8

; FRAME:      FDE cie=00000000 pc=00000000...00000010
; FRAME:      0x0: CFA=SP+8: RA=[CFA-8]
; FRAME-NEXT: 0x4: CFA=R15+16: R15=[CFA-16], RA=[CFA-8]

; FRAME:      FDE cie=00000000 pc=00000010...00000050
; FRAME:      0x10: CFA=SP+8: RA=[CFA-8]
; FRAME-NEXT: 0x14: CFA=R15+16: R15=[CFA-16], RA=[CFA-8]
; FRAME-NEXT: 0x18: CFA=R15+16: R4=[CFA-24], R15=[CFA-16], RA=[CFA-8]
; FRAME-NEXT: 0x1c: CFA=R15+16: R4=[CFA-24], R5=[CFA-32], R15=[CFA-16], RA=[CFA-8]

; LINE:      0x0000000000000000      2      0
; LINE:      0x0000000000000010      3      0
; LINE:      0x0000000000000050      5      3 {{.*}} end_sequence

; VERIFY: No errors.

target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64"
target triple = "klausscpu"

; Function Attrs: nounwind
define dso_local range(i64 -9223372036854775807, -9223372036854775808) i64 @first(i64 noundef %a) local_unnamed_addr #0 !dbg !10 {
entry:
    #dbg_value(i64 %a, !15, !DIExpression(), !16)
  %call = tail call i64 @ext(i64 noundef %a) #2, !dbg !17
  %add = add nsw i64 %call, 1, !dbg !18
  ret i64 %add, !dbg !19
}

declare !dbg !20 dso_local i64 @ext(i64 noundef) local_unnamed_addr #1

; Function Attrs: nounwind
define dso_local i64 @second(i64 noundef %a, i64 noundef %b) local_unnamed_addr #0 !dbg !21 {
entry:
    #dbg_value(i64 %a, !25, !DIExpression(), !29)
    #dbg_value(i64 %b, !26, !DIExpression(), !29)
  %call = tail call i64 @ext(i64 noundef %a) #2, !dbg !30
    #dbg_value(i64 %call, !27, !DIExpression(), !29)
  %call1 = tail call i64 @ext(i64 noundef %b) #2, !dbg !31
    #dbg_value(i64 %call1, !28, !DIExpression(), !29)
  %mul = mul nsw i64 %call1, %call, !dbg !32
  %call2 = tail call i64 @ext(i64 noundef %mul) #2, !dbg !33
  %add = add i64 %call1, %call, !dbg !34
  %add3 = add i64 %add, %call2, !dbg !35
  ret i64 %add3, !dbg !36
}

attributes #0 = { nounwind "frame-pointer"="all" "no-builtins" "no-trapping-math"="true" "stack-protector-buffer-size"="8" }
attributes #1 = { "frame-pointer"="all" "no-builtins" "no-trapping-math"="true" "stack-protector-buffer-size"="8" }
attributes #2 = { nobuiltin nounwind "no-builtins" }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3, !4, !5}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug, splitDebugInlining: false, nameTableKind: None)
!1 = !DIFile(filename: "frame.c", directory: "/src")
!2 = !{i32 7, !"Dwarf Version", i32 5}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !{i32 7, !"frame-pointer", i32 2}
!5 = !{i32 7, !"debug-info-assignment-tracking", i1 true}
!6 = !{!7, !7, i64 0}
!7 = !{!"int", !8, i64 0}
!8 = !{!"omnipotent char", !9, i64 0}
!9 = !{!"Simple C/C++ TBAA"}
!10 = distinct !DISubprogram(name: "first", scope: !1, file: !1, line: 2, type: !11, scopeLine: 2, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !14, keyInstructions: true)
!11 = !DISubroutineType(types: !12)
!12 = !{!13, !13}
!13 = !DIBasicType(name: "long", size: 64, encoding: DW_ATE_signed)
!14 = !{!15}
!15 = !DILocalVariable(name: "a", arg: 1, scope: !10, file: !1, line: 2, type: !13)
!16 = !DILocation(line: 0, scope: !10)
!17 = !DILocation(line: 2, column: 29, scope: !10)
!18 = !DILocation(line: 2, column: 36, scope: !10, atomGroup: 1, atomRank: 2)
!19 = !DILocation(line: 2, column: 22, scope: !10, atomGroup: 1, atomRank: 1)
!20 = !DISubprogram(name: "ext", scope: !1, file: !1, line: 1, type: !11, flags: DIFlagPrototyped, spFlags: DISPFlagOptimized)
!21 = distinct !DISubprogram(name: "second", scope: !1, file: !1, line: 3, type: !22, scopeLine: 3, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !24, keyInstructions: true)
!22 = !DISubroutineType(types: !23)
!23 = !{!13, !13, !13}
!24 = !{!25, !26, !27, !28}
!25 = !DILocalVariable(name: "a", arg: 1, scope: !21, file: !1, line: 3, type: !13)
!26 = !DILocalVariable(name: "b", arg: 2, scope: !21, file: !1, line: 3, type: !13)
!27 = !DILocalVariable(name: "x", scope: !21, file: !1, line: 4, type: !13)
!28 = !DILocalVariable(name: "y", scope: !21, file: !1, line: 4, type: !13)
!29 = !DILocation(line: 0, scope: !21)
!30 = !DILocation(line: 4, column: 12, scope: !21, atomGroup: 1, atomRank: 2)
!31 = !DILocation(line: 4, column: 24, scope: !21, atomGroup: 2, atomRank: 2)
!32 = !DILocation(line: 5, column: 16, scope: !21)
!33 = !DILocation(line: 5, column: 10, scope: !21)
!34 = !DILocation(line: 5, column: 21, scope: !21)
!35 = !DILocation(line: 5, column: 25, scope: !21, atomGroup: 4, atomRank: 2)
!36 = !DILocation(line: 5, column: 3, scope: !21, atomGroup: 4, atomRank: 1)
