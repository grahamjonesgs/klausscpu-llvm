; RUN: llc -O1 -klausscpu-fall-through-align-max-trips=2 -filetype=obj %s -o %t.g.o
; RUN: opt -strip-debug %s -S | llc -O1 -klausscpu-fall-through-align-max-trips=2 -filetype=obj -o %t.n.o
; RUN: llvm-objcopy -O binary -j .text %t.g.o %t.g.bin
; RUN: llvm-objcopy -O binary -j .text %t.n.o %t.n.bin
; RUN: cmp %t.g.bin %t.n.bin
; RUN: llvm-objdump -d --no-show-raw-insn %t.g.o | FileCheck %s

; -g must not change code: KlaussCPUPostIncLoopCmp (the exit test of `up`
; reads the updated counter: addi + fused branch) and KlaussCPULoopTripCount
; (the 2-iteration loop of `two`, with the threshold raised to 2 here, is
; entered by falling into it and gets no alignment NOPs; the recursive loop
; of `fib` likewise) read and write loop
; metadata, which a debug build carries more of (the loop's DILocations).
;
; Generated from (clang -O1 -g, then opt -O1):
;   extern volatile long sink; extern long a[64];
;   void up(long x) { for (long i = x; i < 1000; i += 3) sink = i; }
;   void two(void) { for (int i = 0; i < 2; i++) a[i] = i; }
;   unsigned long fib(unsigned n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }

; CHECK-LABEL: <up>:
; CHECK:         addi r0, r0, 3
; CHECK-NEXT:    fbr r0, r14, 1,
; CHECK-LABEL: <two>:
; CHECK-NOT:     nop
; CHECK:         fbri r12, 1, 0,
; CHECK-LABEL: <fib>:
; CHECK-NOT:     nop
; CHECK:         callrel

target datalayout = "e-m:e-p:64:64-i64:64-n32:64"
target triple = "klausscpu-unknown-unknown-elf"

@sink = external dso_local global i64, align 8
@a = external dso_local local_unnamed_addr global [64 x i64], align 8

; Function Attrs: nofree norecurse nounwind memory(readwrite, argmem: none, target_mem: none)
define dso_local void @up(i64 noundef %x) local_unnamed_addr #0 !dbg !10 {
entry:
    #dbg_value(i64 %x, !15, !DIExpression(), !18)
    #dbg_value(i64 %x, !16, !DIExpression(), !19)
  %cmp3 = icmp slt i64 %x, 1000, !dbg !20
  br i1 %cmp3, label %for.body, label %for.cond.cleanup, !dbg !22

for.cond.cleanup:                                 ; preds = %for.body, %entry
  ret void, !dbg !23

for.body:                                         ; preds = %entry, %for.body
  %i.04 = phi i64 [ %add, %for.body ], [ %x, %entry ]
    #dbg_value(i64 %i.04, !16, !DIExpression(), !19)
  store volatile i64 %i.04, ptr @sink, align 8, !dbg !24, !tbaa !25
  %add = add nsw i64 %i.04, 3, !dbg !27
    #dbg_value(i64 %add, !16, !DIExpression(), !19)
  %cmp = icmp slt i64 %i.04, 997, !dbg !28
  br i1 %cmp, label %for.body, label %for.cond.cleanup, !dbg !29, !llvm.loop !30
}

; Function Attrs: nofree norecurse nosync nounwind memory(write, argmem: none, inaccessiblemem: none, target_mem: none)
define dso_local void @two() local_unnamed_addr #1 !dbg !35 {
entry:
    #dbg_value(i32 0, !39, !DIExpression(), !42)
  br label %for.body, !dbg !43

for.cond.cleanup:                                 ; preds = %for.body
  ret void, !dbg !44

for.body:                                         ; preds = %entry, %for.body
  %indvars.iv = phi i64 [ 0, %entry ], [ %indvars.iv.next, %for.body ]
    #dbg_value(i64 %indvars.iv, !39, !DIExpression(), !42)
  %arrayidx = getelementptr inbounds nuw [8 x i8], ptr @a, i64 %indvars.iv, !dbg !45
  store i64 %indvars.iv, ptr %arrayidx, align 8, !dbg !47, !tbaa !25
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1, !dbg !48
    #dbg_value(i64 %indvars.iv.next, !39, !DIExpression(), !42)
  %cmp = icmp eq i64 %indvars.iv, 0, !dbg !49
  br i1 %cmp, label %for.body, label %for.cond.cleanup, !dbg !50, !llvm.loop !51
}

; Function Attrs: nofree nosync nounwind memory(none)
define dso_local i64 @fib(i32 noundef signext %n) local_unnamed_addr #2 !dbg !54 {
entry:
  br label %tailrecurse

tailrecurse:                                      ; preds = %if.end, %entry
  %accumulator.tr = phi i64 [ 0, %entry ], [ %add, %if.end ]
  %n.tr = phi i32 [ %n, %entry ], [ %sub1, %if.end ]
    #dbg_value(i32 %n.tr, !60, !DIExpression(), !61)
  %cmp = icmp ult i32 %n.tr, 2, !dbg !62
  br i1 %cmp, label %if.then, label %if.end, !dbg !64

if.then:                                          ; preds = %tailrecurse
  %conv = zext nneg i32 %n.tr to i64, !dbg !65
  %accumulator.ret.tr = add i64 %accumulator.tr, %conv
  ret i64 %accumulator.ret.tr, !dbg !66

if.end:                                           ; preds = %tailrecurse
  %sub = add i32 %n.tr, -1, !dbg !67
  %call = tail call i64 @fib(i32 noundef signext %sub), !dbg !68
  %sub1 = add i32 %n.tr, -2, !dbg !69
  %add = add i64 %accumulator.tr, %call, !dbg !70
  br label %tailrecurse, !dbg !71
}

attributes #0 = { nofree norecurse nounwind memory(readwrite, argmem: none, target_mem: none) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" }
attributes #1 = { nofree norecurse nosync nounwind memory(write, argmem: none, inaccessiblemem: none, target_mem: none) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" }
attributes #2 = { nofree nosync nounwind memory(none) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3, !4}
!llvm.ident = !{!5}
!llvm.errno.tbaa = !{!6}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "clang version 23.0.0git (https://github.com/grahamjonesgs/klausscpu-llvm.git 5f97b29a8c0bbf72f8d77e81c77c3e9f100ba0cb)", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug, splitDebugInlining: false, nameTableKind: None)
!1 = !DIFile(filename: "dbg.c", directory: "/tmp/claude-1000/-home-graham-Documents-src-klausscpu-llvm/84942f05-0423-4bf0-b210-609527069cfa/scratchpad/p9/loop", checksumkind: CSK_MD5, checksum: "0895cbea309cbc930e05516803e6ffcc")
!2 = !{i32 7, !"Dwarf Version", i32 5}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !{i32 7, !"frame-pointer", i32 1}
!5 = !{!"clang version 23.0.0git (https://github.com/grahamjonesgs/klausscpu-llvm.git 5f97b29a8c0bbf72f8d77e81c77c3e9f100ba0cb)"}
!6 = !{!7, !7, i64 0}
!7 = !{!"int", !8, i64 0}
!8 = !{!"omnipotent char", !9, i64 0}
!9 = !{!"Simple C/C++ TBAA"}
!10 = distinct !DISubprogram(name: "up", scope: !1, file: !1, line: 3, type: !11, scopeLine: 3, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !14, keyInstructions: true)
!11 = !DISubroutineType(types: !12)
!12 = !{null, !13}
!13 = !DIBasicType(name: "long", size: 64, encoding: DW_ATE_signed)
!14 = !{!15, !16}
!15 = !DILocalVariable(name: "x", arg: 1, scope: !10, file: !1, line: 3, type: !13)
!16 = !DILocalVariable(name: "i", scope: !17, file: !1, line: 3, type: !13)
!17 = distinct !DILexicalBlock(scope: !10, file: !1, line: 3, column: 19)
!18 = !DILocation(line: 0, scope: !10)
!19 = !DILocation(line: 0, scope: !17)
!20 = !DILocation(line: 3, column: 38, scope: !21, atomGroup: 8, atomRank: 1)
!21 = distinct !DILexicalBlock(scope: !17, file: !1, line: 3, column: 19)
!22 = !DILocation(line: 3, column: 19, scope: !17, atomGroup: 9, atomRank: 1)
!23 = !DILocation(line: 3, column: 64, scope: !10, atomGroup: 7, atomRank: 1)
!24 = !DILocation(line: 3, column: 59, scope: !21, atomGroup: 4, atomRank: 1)
!25 = !{!26, !26, i64 0}
!26 = !{!"long", !8, i64 0}
!27 = !DILocation(line: 3, column: 48, scope: !21, atomGroup: 5, atomRank: 2)
!28 = !DILocation(line: 3, column: 38, scope: !21, atomGroup: 2, atomRank: 1)
!29 = !DILocation(line: 3, column: 19, scope: !17, atomGroup: 3, atomRank: 1)
!30 = distinct !{!30, !31, !32, !33, !34}
!31 = !DILocation(line: 3, column: 19, scope: !17)
!32 = !DILocation(line: 3, column: 61, scope: !17)
!33 = !{!"llvm.loop.mustprogress"}
!34 = !{!"llvm.loop.unroll.disable"}
!35 = distinct !DISubprogram(name: "two", scope: !1, file: !1, line: 4, type: !36, scopeLine: 4, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !38, keyInstructions: true)
!36 = !DISubroutineType(types: !37)
!37 = !{null}
!38 = !{!39}
!39 = !DILocalVariable(name: "i", scope: !40, file: !1, line: 4, type: !41)
!40 = distinct !DILexicalBlock(scope: !35, file: !1, line: 4, column: 18)
!41 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!42 = !DILocation(line: 0, scope: !40)
!43 = !DILocation(line: 4, column: 18, scope: !40, atomGroup: 11, atomRank: 1)
!44 = !DILocation(line: 4, column: 56, scope: !35, atomGroup: 7, atomRank: 1)
!45 = !DILocation(line: 4, column: 46, scope: !46)
!46 = distinct !DILexicalBlock(scope: !40, file: !1, line: 4, column: 18)
!47 = !DILocation(line: 4, column: 51, scope: !46, atomGroup: 4, atomRank: 1)
!48 = !DILocation(line: 4, column: 42, scope: !46, atomGroup: 5, atomRank: 2)
!49 = !DILocation(line: 4, column: 36, scope: !46, atomGroup: 2, atomRank: 1)
!50 = !DILocation(line: 4, column: 18, scope: !40, atomGroup: 3, atomRank: 1)
!51 = distinct !{!51, !52, !53, !33, !34}
!52 = !DILocation(line: 4, column: 18, scope: !40)
!53 = !DILocation(line: 4, column: 53, scope: !40)
!54 = distinct !DISubprogram(name: "fib", scope: !1, file: !1, line: 5, type: !55, scopeLine: 5, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !59, keyInstructions: true)
!55 = !DISubroutineType(types: !56)
!56 = !{!57, !58}
!57 = !DIBasicType(name: "unsigned long", size: 64, encoding: DW_ATE_unsigned)
!58 = !DIBasicType(name: "unsigned int", size: 32, encoding: DW_ATE_unsigned)
!59 = !{!60}
!60 = !DILocalVariable(name: "n", arg: 1, scope: !54, file: !1, line: 5, type: !58)
!61 = !DILocation(line: 0, scope: !54)
!62 = !DILocation(line: 5, column: 39, scope: !63, atomGroup: 1, atomRank: 2)
!63 = distinct !DILexicalBlock(scope: !54, file: !1, line: 5, column: 37)
!64 = !DILocation(line: 5, column: 39, scope: !63, atomGroup: 1, atomRank: 1)
!65 = !DILocation(line: 5, column: 51, scope: !63, atomGroup: 2, atomRank: 2)
!66 = !DILocation(line: 5, column: 86, scope: !54, atomGroup: 4, atomRank: 1)
!67 = !DILocation(line: 5, column: 67, scope: !54)
!68 = !DILocation(line: 5, column: 61, scope: !54)
!69 = !DILocation(line: 5, column: 80, scope: !54)
!70 = !DILocation(line: 5, column: 72, scope: !54, atomGroup: 3, atomRank: 2)
!71 = !DILocation(line: 5, column: 74, scope: !54)
