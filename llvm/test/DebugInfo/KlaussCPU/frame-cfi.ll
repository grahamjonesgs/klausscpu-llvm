; RUN: llc -O1 -filetype=obj < %s -o %t.o
; RUN: llvm-dwarfdump --debug-frame %t.o | FileCheck %s --check-prefix=FRAME
; RUN: llvm-dwarfdump --debug-info %t.o | FileCheck %s --check-prefix=INFO
; RUN: llvm-objdump -d --no-show-raw-insn %t.o | FileCheck %s --check-prefix=ASM
; RUN: llvm-dwarfdump --verify %t.o | FileCheck %s --check-prefix=VERIFY

; The unwind rows of the frames this backend no longer always builds (clang's
; default "frame-pointer"="non-leaf"): a frameless leaf and a frameless tail
; call keep the CIE's CFA = SP+8 throughout; a shrink-wrapped function is
; frameless until its ENTER and again on the early-exit path laid out after
; its LEAVERET (CFIFixup); a framed tail call is back in the CIE's state,
; with R15 and R4 restored, from its LEAVE to the jump (the 1-word `tail`).  A frameless function
; has SP as its frame base.  The tail calls are DW_AT_call_tail_call sites.
;
; Generated from (clang -O1 -g):
;   long ext(long);
;   long leaf(long a, long b) { return a * b + 1; }
;   long early_exit(long *p) {
;     if (!p)
;       return 0;
;     return ext(*p) + 1;
;   }
;   long tail_framed(long a, long b) {
;     long x = ext(a);
;     return ext(x + b);
;   }
;   long tail_frameless(long a) { return ext(a + 2); }

; ASM:      <leaf>:
; ASM-NEXT:    0: mulr r12, r1, r0
; ASM-NEXT:    4: incr r12, r12
; ASM-NEXT:    8: ret
; ASM:      <early_exit>:
; ASM-NEXT:    c: fbri r0, 0, 0, 0x24
; ASM-NEXT:   10: enter 0
; ASM-NEXT:   14: ldidx64 r0, r0, 0
; ASM-NEXT:   18: callrel
; ASM-NEXT:   1c: incr r12, r12
; ASM-NEXT:   20: leaveret
; ASM-NEXT:   24: setr r12, 0
; ASM-NEXT:   28: ret
; ASM:      <tail_framed>:
; ASM-NEXT:   2c: enter 1
; ASM-NEXT:   30: stidx64 r4, r15, -8
; ASM-NEXT:   34: copy r4, r1
; ASM-NEXT:   38: callrel
; ASM-NEXT:   3c: addr r0, r12, r4
; ASM-NEXT:   40: ldidx64 r4, r15, -8
; ASM-NEXT:   44: leave
; ASM-NEXT:   48: jmprel
; ASM:      <tail_frameless>:
; ASM-NEXT:   4c: addi r0, r0, 2
; ASM-NEXT:   50: jmprel

; FRAME:      FDE cie=00000000 pc=00000000...0000000c
; FRAME:      0x0: CFA=SP+8: RA=[CFA-8]
; FRAME-EMPTY:
; FRAME-NEXT: FDE cie=00000000 pc=0000000c...0000002c
; FRAME:      0xc: CFA=SP+8: RA=[CFA-8]
; FRAME-NEXT: 0x14: CFA=R15+16: R15=[CFA-16], RA=[CFA-8]
; FRAME-NEXT: 0x24: CFA=SP+8: R15=same, RA=[CFA-8]
; FRAME-EMPTY:
; FRAME-NEXT: FDE cie=00000000 pc=0000002c...0000004c
; FRAME:      0x2c: CFA=SP+8: RA=[CFA-8]
; FRAME-NEXT: 0x30: CFA=R15+16: R15=[CFA-16], RA=[CFA-8]
; FRAME-NEXT: 0x34: CFA=R15+16: R4=[CFA-24], R15=[CFA-16], RA=[CFA-8]
; FRAME-NEXT: 0x48: CFA=SP+8: R4=same, R15=same, RA=[CFA-8]
; FRAME-EMPTY:
; FRAME-NEXT: FDE cie=00000000 pc=0000004c...00000054
; FRAME:      0x4c: CFA=SP+8: RA=[CFA-8]
; FRAME-EMPTY:

; INFO:       DW_AT_frame_base (DW_OP_reg16 SP)
; INFO-NEXT:  DW_AT_call_all_calls (true)
; INFO-NEXT:  DW_AT_name ("leaf")
; INFO:       DW_AT_frame_base (DW_OP_reg15 R15)
; INFO-NEXT:  DW_AT_call_all_calls (true)
; INFO-NEXT:  DW_AT_name ("early_exit")
; INFO:       DW_AT_frame_base (DW_OP_reg15 R15)
; INFO-NEXT:  DW_AT_call_all_calls (true)
; INFO-NEXT:  DW_AT_name ("tail_framed")
; INFO:       DW_TAG_call_site
; INFO-NEXT:    DW_AT_call_origin
; INFO-NEXT:    DW_AT_call_return_pc (0x0000003c)
; INFO:       DW_TAG_call_site
; INFO-NEXT:    DW_AT_call_origin
; INFO-NEXT:    DW_AT_call_tail_call (true)
; INFO-NEXT:    DW_AT_call_pc (0x00000048)
; INFO:       DW_AT_frame_base (DW_OP_reg16 SP)
; INFO-NEXT:  DW_AT_call_all_calls (true)
; INFO-NEXT:  DW_AT_name ("tail_frameless")
; INFO:       DW_TAG_call_site
; INFO-NEXT:    DW_AT_call_origin
; INFO-NEXT:    DW_AT_call_tail_call (true)
; INFO-NEXT:    DW_AT_call_pc (0x00000050)

; VERIFY: No errors.

target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64"
target triple = "klausscpu-unknown-unknown-elf"
; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define dso_local range(i64 -9223372036854775807, -9223372036854775808) i64 @leaf(i64 noundef %a, i64 noundef %b) local_unnamed_addr #0 !dbg !11 {
entry:
    #dbg_value(i64 %a, !16, !DIExpression(), !18)
    #dbg_value(i64 %b, !17, !DIExpression(), !18)
  %mul = mul nsw i64 %b, %a, !dbg !19
  %add = add nsw i64 %mul, 1, !dbg !20
  ret i64 %add, !dbg !21
}
; Function Attrs: nounwind
define dso_local range(i64 -9223372036854775807, -9223372036854775808) i64 @early_exit(ptr noundef readonly captures(address_is_null) %p) local_unnamed_addr #1 !dbg !22 {
entry:
    #dbg_value(ptr %p, !27, !DIExpression(), !28)
  %tobool.not = icmp eq ptr %p, null, !dbg !29
  br i1 %tobool.not, label %return, label %if.end, !dbg !31
if.end:                                           ; preds = %entry
  %0 = load i64, ptr %p, align 8, !dbg !32, !tbaa !33
  %call = tail call i64 @ext(i64 noundef %0) #3, !dbg !35
  %add = add nsw i64 %call, 1, !dbg !36
  br label %return, !dbg !37
return:                                           ; preds = %entry, %if.end
  %retval.0 = phi i64 [ %add, %if.end ], [ 0, %entry ], !dbg !28
  ret i64 %retval.0, !dbg !38
}
declare !dbg !39 dso_local i64 @ext(i64 noundef) local_unnamed_addr #2
; Function Attrs: nounwind
define dso_local i64 @tail_framed(i64 noundef %a, i64 noundef %b) local_unnamed_addr #1 !dbg !42 {
entry:
    #dbg_value(i64 %a, !44, !DIExpression(), !47)
    #dbg_value(i64 %b, !45, !DIExpression(), !47)
  %call = tail call i64 @ext(i64 noundef %a) #3, !dbg !48
    #dbg_value(i64 %call, !46, !DIExpression(), !47)
  %add = add nsw i64 %call, %b, !dbg !49
  %call1 = tail call i64 @ext(i64 noundef %add) #3, !dbg !50
  ret i64 %call1, !dbg !51
}
; Function Attrs: nounwind
define dso_local i64 @tail_frameless(i64 noundef %a) local_unnamed_addr #1 !dbg !52 {
entry:
    #dbg_value(i64 %a, !54, !DIExpression(), !55)
  %add = add nsw i64 %a, 2, !dbg !56
  %call = tail call i64 @ext(i64 noundef %add) #3, !dbg !57
  ret i64 %call, !dbg !58
}
attributes #0 = { mustprogress nofree norecurse nosync nounwind willreturn memory(none) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" }
attributes #1 = { nounwind "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" }
attributes #2 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" }
attributes #3 = { nounwind }
!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3, !4, !5}
!llvm.errno.tbaa = !{!7}
!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "clang version 23.0.0git (https://github.com/grahamjonesgs/klausscpu-llvm.git 100b193e22613d905c13bd38983306e2fc326bf7)", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug, splitDebugInlining: false, nameTableKind: None)
!1 = !DIFile(filename: "fcfi.c", directory: "/tmp", checksumkind: CSK_MD5, checksum: "a483536b5c52d9917b4116edbb10cc58")
!2 = !{i32 7, !"Dwarf Version", i32 5}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !{i32 7, !"frame-pointer", i32 1}
!5 = !{i32 7, !"debug-info-assignment-tracking", i1 true}
!7 = !{!8, !8, i64 0}
!8 = !{!"int", !9, i64 0}
!9 = !{!"omnipotent char", !10, i64 0}
!10 = !{!"Simple C/C++ TBAA"}
!11 = distinct !DISubprogram(name: "leaf", scope: !1, file: !1, line: 2, type: !12, scopeLine: 2, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !15, keyInstructions: true)
!12 = !DISubroutineType(types: !13)
!13 = !{!14, !14, !14}
!14 = !DIBasicType(name: "long", size: 64, encoding: DW_ATE_signed)
!15 = !{!16, !17}
!16 = !DILocalVariable(name: "a", arg: 1, scope: !11, file: !1, line: 2, type: !14)
!17 = !DILocalVariable(name: "b", arg: 2, scope: !11, file: !1, line: 2, type: !14)
!18 = !DILocation(line: 0, scope: !11)
!19 = !DILocation(line: 2, column: 38, scope: !11)
!20 = !DILocation(line: 2, column: 42, scope: !11, atomGroup: 1, atomRank: 2)
!21 = !DILocation(line: 2, column: 29, scope: !11, atomGroup: 1, atomRank: 1)
!22 = distinct !DISubprogram(name: "early_exit", scope: !1, file: !1, line: 3, type: !23, scopeLine: 3, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !26, keyInstructions: true)
!23 = !DISubroutineType(types: !24)
!24 = !{!14, !25}
!25 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !14, size: 64)
!26 = !{!27}
!27 = !DILocalVariable(name: "p", arg: 1, scope: !22, file: !1, line: 3, type: !25)
!28 = !DILocation(line: 0, scope: !22)
!29 = !DILocation(line: 4, column: 8, scope: !30, atomGroup: 1, atomRank: 2)
!30 = distinct !DILexicalBlock(scope: !22, file: !1, line: 4, column: 7)
!31 = !DILocation(line: 4, column: 7, scope: !30, atomGroup: 1, atomRank: 1)
!32 = !DILocation(line: 6, column: 14, scope: !22)
!33 = !{!34, !34, i64 0}
!34 = !{!"long", !9, i64 0}
!35 = !DILocation(line: 6, column: 10, scope: !22)
!36 = !DILocation(line: 6, column: 18, scope: !22, atomGroup: 3, atomRank: 2)
!37 = !DILocation(line: 6, column: 3, scope: !22, atomGroup: 3, atomRank: 1)
!38 = !DILocation(line: 7, column: 1, scope: !22, atomGroup: 4, atomRank: 1)
!39 = !DISubprogram(name: "ext", scope: !1, file: !1, line: 1, type: !40, flags: DIFlagPrototyped, spFlags: DISPFlagOptimized)
!40 = !DISubroutineType(types: !41)
!41 = !{!14, !14}
!42 = distinct !DISubprogram(name: "tail_framed", scope: !1, file: !1, line: 8, type: !12, scopeLine: 8, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !43, keyInstructions: true)
!43 = !{!44, !45, !46}
!44 = !DILocalVariable(name: "a", arg: 1, scope: !42, file: !1, line: 8, type: !14)
!45 = !DILocalVariable(name: "b", arg: 2, scope: !42, file: !1, line: 8, type: !14)
!46 = !DILocalVariable(name: "x", scope: !42, file: !1, line: 9, type: !14)
!47 = !DILocation(line: 0, scope: !42)
!48 = !DILocation(line: 9, column: 12, scope: !42, atomGroup: 1, atomRank: 2)
!49 = !DILocation(line: 10, column: 16, scope: !42)
!50 = !DILocation(line: 10, column: 10, scope: !42, atomGroup: 3, atomRank: 2)
!51 = !DILocation(line: 10, column: 3, scope: !42, atomGroup: 3, atomRank: 1)
!52 = distinct !DISubprogram(name: "tail_frameless", scope: !1, file: !1, line: 12, type: !40, scopeLine: 12, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !53, keyInstructions: true)
!53 = !{!54}
!54 = !DILocalVariable(name: "a", arg: 1, scope: !52, file: !1, line: 12, type: !14)
!55 = !DILocation(line: 0, scope: !52)
!56 = !DILocation(line: 12, column: 44, scope: !52)
!57 = !DILocation(line: 12, column: 38, scope: !52, atomGroup: 1, atomRank: 2)
!58 = !DILocation(line: 12, column: 31, scope: !52, atomGroup: 1, atomRank: 1)
