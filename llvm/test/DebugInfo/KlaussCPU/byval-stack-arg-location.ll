; RUN: llc -O0 -verify-machineinstrs < %s | FileCheck %s --check-prefix=ASM
; RUN: llc -O0 -filetype=obj < %s -o %t.o
; RUN: llvm-dwarfdump --debug-info %t.o | FileCheck %s --check-prefix=INFO
; RUN: llvm-dwarfdump --verify %t.o | FileCheck %s --check-prefix=VERIFY

; A byval struct argument is passed as a pointer to a caller-made copy.  When
; that pointer arrives on the stack, the callee's fixed slot holds the
; struct's address, not the struct, so the location must dereference the slot
; (DW_OP_fbreg +40, DW_OP_deref).  SelectionDAG used to describe the
; dbg.declare as living in the slot itself, so the debugger showed the pointer
; and the following stack words as the struct's fields.  A byval pointer that
; arrives in a register is described by DW_OP_breg0.
;
; Generated from (clang -O0 -g):
;   struct big { long v[6]; };
;   void use(void *);
;   long r(struct big bb, long e) { long t = bb.v[2] + e; use(&t); return t; }
;   long s(long a, long b, long c, long d, struct big bb) {
;     long t = bb.v[2]; use(&t); return t;
;   }

; ASM-LABEL: r:
; ASM:       #DEBUG_VALUE: r:bb <- [$r0+0]
; ASM-LABEL: s:
; ASM:       #DEBUG_VALUE: s:bb <- [DW_OP_plus_uconst 40, DW_OP_deref] [$r15+0]
; ASM-NEXT:  ldidx64 r12, r15, 40
; ASM:       ldidx64 r12, r12, 16

; INFO:      DW_AT_name ("r")
; INFO:      DW_TAG_formal_parameter
; INFO-NEXT:   DW_AT_location
; INFO-NEXT:     [{{.*}}): DW_OP_breg0 R0+0)
; INFO-NEXT:   DW_AT_name ("bb")
; INFO:      DW_AT_name ("s")
; INFO:      DW_TAG_formal_parameter
; INFO-NEXT:   DW_AT_location (DW_OP_fbreg -8)
; INFO-NEXT:   DW_AT_name ("a")
; INFO:        DW_AT_location (DW_OP_fbreg +40, DW_OP_deref)
; INFO-NEXT:   DW_AT_name ("bb")

; VERIFY: No errors.

target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64"
target triple = "klausscpu-unknown-unknown-elf"

%struct.big = type { [6 x i64] }

; Function Attrs: noinline nounwind optnone
define i64 @r(ptr noundef byval(%struct.big) align 8 %bb, i64 noundef %e) #0 !dbg !7 {
entry:
  %e.addr = alloca i64, align 8
  %t = alloca i64, align 8
    #dbg_declare(ptr %bb, !18, !DIExpression(), !19)
  store i64 %e, ptr %e.addr, align 8
    #dbg_declare(ptr %e.addr, !20, !DIExpression(), !21)
    #dbg_declare(ptr %t, !22, !DIExpression(), !23)
  %v = getelementptr inbounds nuw %struct.big, ptr %bb, i32 0, i32 0, !dbg !24
  %arrayidx = getelementptr inbounds [6 x i64], ptr %v, i64 0, i64 2, !dbg !25
  %0 = load i64, ptr %arrayidx, align 8, !dbg !25
  %1 = load i64, ptr %e.addr, align 8, !dbg !26
  %add = add nsw i64 %0, %1, !dbg !27
  store i64 %add, ptr %t, align 8, !dbg !23
  call void @use(ptr noundef %t) #2, !dbg !28
  %2 = load i64, ptr %t, align 8, !dbg !29
  ret i64 %2, !dbg !30
}

declare void @use(ptr noundef) #1

; Function Attrs: noinline nounwind optnone
define i64 @s(i64 noundef %a, i64 noundef %b, i64 noundef %c, i64 noundef %d, ptr noundef byval(%struct.big) align 8 %bb) #0 !dbg !31 {
entry:
  %a.addr = alloca i64, align 8
  %b.addr = alloca i64, align 8
  %c.addr = alloca i64, align 8
  %d.addr = alloca i64, align 8
  %t = alloca i64, align 8
  store i64 %a, ptr %a.addr, align 8
    #dbg_declare(ptr %a.addr, !34, !DIExpression(), !35)
  store i64 %b, ptr %b.addr, align 8
    #dbg_declare(ptr %b.addr, !36, !DIExpression(), !37)
  store i64 %c, ptr %c.addr, align 8
    #dbg_declare(ptr %c.addr, !38, !DIExpression(), !39)
  store i64 %d, ptr %d.addr, align 8
    #dbg_declare(ptr %d.addr, !40, !DIExpression(), !41)
    #dbg_declare(ptr %bb, !42, !DIExpression(), !43)
    #dbg_declare(ptr %t, !44, !DIExpression(), !45)
  %v = getelementptr inbounds nuw %struct.big, ptr %bb, i32 0, i32 0, !dbg !46
  %arrayidx = getelementptr inbounds [6 x i64], ptr %v, i64 0, i64 2, !dbg !47
  %0 = load i64, ptr %arrayidx, align 8, !dbg !47
  store i64 %0, ptr %t, align 8, !dbg !45
  call void @use(ptr noundef %t) #2, !dbg !48
  %1 = load i64, ptr %t, align 8, !dbg !49
  ret i64 %1, !dbg !50
}

attributes #0 = { noinline nounwind optnone "frame-pointer"="all" }
attributes #1 = { "frame-pointer"="all" }
attributes #2 = { nobuiltin }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3, !5}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, splitDebugInlining: false, nameTableKind: None)
!1 = !DIFile(filename: "bv.c", directory: "/tmp")
!2 = !{i32 7, !"Dwarf Version", i32 5}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!5 = !{i32 7, !"frame-pointer", i32 2}
!7 = distinct !DISubprogram(name: "r", scope: !1, file: !1, line: 3, type: !8, scopeLine: 3, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !17)
!8 = !DISubroutineType(types: !9)
!9 = !{!10, !11, !10}
!10 = !DIBasicType(name: "long", size: 64, encoding: DW_ATE_signed)
!11 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "big", file: !1, line: 1, size: 384, elements: !12)
!12 = !{!13}
!13 = !DIDerivedType(tag: DW_TAG_member, name: "v", scope: !11, file: !1, line: 1, baseType: !14, size: 384)
!14 = !DICompositeType(tag: DW_TAG_array_type, baseType: !10, size: 384, elements: !15)
!15 = !{!16}
!16 = !DISubrange(count: 6)
!17 = !{}
!18 = !DILocalVariable(name: "bb", arg: 1, scope: !7, file: !1, line: 3, type: !11)
!19 = !DILocation(line: 3, column: 19, scope: !7)
!20 = !DILocalVariable(name: "e", arg: 2, scope: !7, file: !1, line: 3, type: !10)
!21 = !DILocation(line: 3, column: 28, scope: !7)
!22 = !DILocalVariable(name: "t", scope: !7, file: !1, line: 3, type: !10)
!23 = !DILocation(line: 3, column: 38, scope: !7)
!24 = !DILocation(line: 3, column: 45, scope: !7)
!25 = !DILocation(line: 3, column: 42, scope: !7)
!26 = !DILocation(line: 3, column: 52, scope: !7)
!27 = !DILocation(line: 3, column: 50, scope: !7)
!28 = !DILocation(line: 3, column: 55, scope: !7)
!29 = !DILocation(line: 3, column: 71, scope: !7)
!30 = !DILocation(line: 3, column: 64, scope: !7)
!31 = distinct !DISubprogram(name: "s", scope: !1, file: !1, line: 4, type: !32, scopeLine: 4, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !17)
!32 = !DISubroutineType(types: !33)
!33 = !{!10, !10, !10, !10, !10, !11}
!34 = !DILocalVariable(name: "a", arg: 1, scope: !31, file: !1, line: 4, type: !10)
!35 = !DILocation(line: 4, column: 13, scope: !31)
!36 = !DILocalVariable(name: "b", arg: 2, scope: !31, file: !1, line: 4, type: !10)
!37 = !DILocation(line: 4, column: 21, scope: !31)
!38 = !DILocalVariable(name: "c", arg: 3, scope: !31, file: !1, line: 4, type: !10)
!39 = !DILocation(line: 4, column: 29, scope: !31)
!40 = !DILocalVariable(name: "d", arg: 4, scope: !31, file: !1, line: 4, type: !10)
!41 = !DILocation(line: 4, column: 37, scope: !31)
!42 = !DILocalVariable(name: "bb", arg: 5, scope: !31, file: !1, line: 4, type: !11)
!43 = !DILocation(line: 4, column: 51, scope: !31)
!44 = !DILocalVariable(name: "t", scope: !31, file: !1, line: 4, type: !10)
!45 = !DILocation(line: 4, column: 62, scope: !31)
!46 = !DILocation(line: 4, column: 69, scope: !31)
!47 = !DILocation(line: 4, column: 66, scope: !31)
!48 = !DILocation(line: 4, column: 75, scope: !31)
!49 = !DILocation(line: 4, column: 91, scope: !31)
!50 = !DILocation(line: 4, column: 84, scope: !31)
