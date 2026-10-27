; RUN: llc -O1 -verify-machineinstrs -print-after=klausscpu-iv-inc \
; RUN:   -filetype=null < %s 2>&1 | FileCheck %s --check-prefix=IR
; RUN: llc -O1 -filetype=obj < %s -o %t.o
; RUN: llvm-dwarfdump --debug-info %t.o | FileCheck %s --check-prefix=INFO
; RUN: llvm-dwarfdump --verify %t.o | FileCheck %s --check-prefix=VERIFY

; KlaussCPUEarlyIVInc replaces a loop counter's PHI and increment.  The
; counter must keep its debug location: %i's dbg_values move to the new add,
; with a copy as %i.prev + C at the top of the block (until the add issues),
; and %i.next's become the new add + C.  The pass used to leave them all
; poison, so 'i' had no DW_AT_location at all.
;
; Generated from (clang -O1 -g, then trimmed):
;   long up(long b) {
;     for (long i = 0; i < 100000; i++)
;       b = (b ^ (b << 3)) + i;
;     return b;
;   }
;   long down(long b) {
;     for (long i = 1000; i != 0; i--)
;       b = (b ^ (b << 3)) + i;
;     return b;
;   }

; IR-LABEL: define i64 @up(
; IR:       for.body:
; IR-NEXT:    %i.prev = phi i64 [ -1, %entry ], [ %i1, %for.body ]
; IR-NEXT:    %b.addr = phi i64
; IR-NEXT:      #dbg_value(i64 %i.prev, [[UP_I:![0-9]+]], !DIExpression(DW_OP_plus_uconst, 1, DW_OP_stack_value),
; IR-NEXT:    %i1 = add i64 %i.prev, 1
; IR-NEXT:      #dbg_value(i64 %i1, [[UP_I]], !DIExpression(),
; IR:         %add = add nsw i64 %xor, %i1
; IR-NEXT:      #dbg_value(i64 %add,
; IR-NEXT:      #dbg_value(i64 %i1, [[UP_I]], !DIExpression(DW_OP_plus_uconst, 1, DW_OP_stack_value),
; IR-NEXT:    %exitcond = icmp eq i64 %i1, 99999
; IR-NOT:   poison

; IR-LABEL: define i64 @down(
; IR:       for.body:
; IR-NEXT:    %i.prev = phi i64 [ 1001, %entry ], [ %i1, %for.body ]
; IR-NEXT:    %b.addr = phi i64
; IR-NEXT:      #dbg_value(i64 %i.prev, [[DOWN_I:![0-9]+]], !DIExpression(DW_OP_constu, 1, DW_OP_minus, DW_OP_stack_value),
; IR-NEXT:    %i1 = add i64 %i.prev, -1
; IR-NEXT:      #dbg_value(i64 %i1, [[DOWN_I]], !DIExpression(),
; IR:         %add = add nsw i64 %xor, %i1
; IR-NEXT:      #dbg_value(i64 %add,
; IR-NEXT:      #dbg_value(i64 %i1, [[DOWN_I]], !DIExpression(DW_OP_constu, 1, DW_OP_minus, DW_OP_stack_value),
; IR-NEXT:    %exitcond = icmp eq i64 %i1, 1
; IR-NOT:   poison

; 'i' has a location from the loop's top to its end: the old counter's
; register + C until the INCR/DECR, then the register, then + C again after
; the old increment's place.
; INFO:      DW_AT_name ("up")
; INFO:      DW_AT_location (indexed ({{.*}}) loclist = {{.*}}:
; INFO-NEXT:   [0x{{[0-9a-f]+}}, 0x[[UP1:[0-9a-f]+]]): DW_OP_breg12 R12+1, DW_OP_stack_value
; INFO-NEXT:   [0x[[UP1]], 0x[[UP2:[0-9a-f]+]]): DW_OP_reg12 R12
; INFO-NEXT:   [0x[[UP2]], 0x{{[0-9a-f]+}}): DW_OP_breg12 R12+1, DW_OP_stack_value)
; INFO-NEXT: DW_AT_name ("i")
; INFO:      DW_AT_name ("down")
; INFO:      DW_AT_location (indexed ({{.*}}) loclist = {{.*}}:
; INFO-NEXT:   [0x{{[0-9a-f]+}}, 0x[[DN1:[0-9a-f]+]]): DW_OP_breg12 R12-1, DW_OP_stack_value
; INFO-NEXT:   [0x[[DN1]], 0x[[DN2:[0-9a-f]+]]): DW_OP_reg12 R12
; INFO-NEXT:   [0x[[DN2]], 0x{{[0-9a-f]+}}): DW_OP_breg12 R12-1, DW_OP_stack_value)
; INFO-NEXT: DW_AT_name ("i")

; VERIFY: No errors.

target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64"
target triple = "klausscpu-unknown-none-elf"

define i64 @up(i64 %b) nounwind !dbg !10 {
entry:
    #dbg_value(i64 %b, !15, !DIExpression(), !18)
    #dbg_value(i64 0, !16, !DIExpression(), !19)
  br label %for.body, !dbg !20

for.cond.cleanup:
  ret i64 %add, !dbg !21

for.body:
  %i = phi i64 [ 0, %entry ], [ %inc, %for.body ]
  %b.addr = phi i64 [ %b, %entry ], [ %add, %for.body ]
    #dbg_value(i64 %i, !16, !DIExpression(), !19)
    #dbg_value(i64 %b.addr, !15, !DIExpression(), !18)
  %shl = shl i64 %b.addr, 3, !dbg !22
  %xor = xor i64 %shl, %b.addr, !dbg !22
  %add = add nsw i64 %xor, %i, !dbg !22
    #dbg_value(i64 %add, !15, !DIExpression(), !18)
  %inc = add nuw nsw i64 %i, 1, !dbg !20
    #dbg_value(i64 %inc, !16, !DIExpression(), !19)
  %exitcond = icmp eq i64 %inc, 100000, !dbg !20
  br i1 %exitcond, label %for.cond.cleanup, label %for.body, !dbg !20
}

define i64 @down(i64 %b) nounwind !dbg !30 {
entry:
    #dbg_value(i64 %b, !32, !DIExpression(), !35)
    #dbg_value(i64 1000, !33, !DIExpression(), !36)
  br label %for.body, !dbg !37

for.cond.cleanup:
  ret i64 %add, !dbg !38

for.body:
  %i = phi i64 [ 1000, %entry ], [ %dec, %for.body ]
  %b.addr = phi i64 [ %b, %entry ], [ %add, %for.body ]
    #dbg_value(i64 %i, !33, !DIExpression(), !36)
    #dbg_value(i64 %b.addr, !32, !DIExpression(), !35)
  %shl = shl i64 %b.addr, 3, !dbg !39
  %xor = xor i64 %shl, %b.addr, !dbg !39
  %add = add nsw i64 %xor, %i, !dbg !39
    #dbg_value(i64 %add, !32, !DIExpression(), !35)
  %dec = add nsw i64 %i, -1, !dbg !37
    #dbg_value(i64 %dec, !33, !DIExpression(), !36)
  %exitcond = icmp eq i64 %dec, 0, !dbg !37
  br i1 %exitcond, label %for.cond.cleanup, label %for.body, !dbg !37
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "clang", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "early-iv-inc.c", directory: "/")
!2 = !{i32 7, !"Dwarf Version", i32 5}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!10 = distinct !DISubprogram(name: "up", scope: !1, file: !1, line: 1, type: !11, scopeLine: 1, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !14)
!11 = !DISubroutineType(types: !12)
!12 = !{!13, !13}
!13 = !DIBasicType(name: "long", size: 64, encoding: DW_ATE_signed)
!14 = !{!15, !16}
!15 = !DILocalVariable(name: "b", arg: 1, scope: !10, file: !1, line: 1, type: !13)
!16 = !DILocalVariable(name: "i", scope: !17, file: !1, line: 2, type: !13)
!17 = distinct !DILexicalBlock(scope: !10, file: !1, line: 2, column: 3)
!18 = !DILocation(line: 0, scope: !10)
!19 = !DILocation(line: 0, scope: !17)
!20 = !DILocation(line: 2, column: 3, scope: !17)
!21 = !DILocation(line: 4, column: 3, scope: !10)
!22 = !DILocation(line: 3, column: 7, scope: !17)
!30 = distinct !DISubprogram(name: "down", scope: !1, file: !1, line: 6, type: !11, scopeLine: 6, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !31)
!31 = !{!32, !33}
!32 = !DILocalVariable(name: "b", arg: 1, scope: !30, file: !1, line: 6, type: !13)
!33 = !DILocalVariable(name: "i", scope: !34, file: !1, line: 7, type: !13)
!34 = distinct !DILexicalBlock(scope: !30, file: !1, line: 7, column: 3)
!35 = !DILocation(line: 0, scope: !30)
!36 = !DILocation(line: 0, scope: !34)
!37 = !DILocation(line: 7, column: 3, scope: !34)
!38 = !DILocation(line: 9, column: 3, scope: !30)
!39 = !DILocation(line: 8, column: 7, scope: !34)
