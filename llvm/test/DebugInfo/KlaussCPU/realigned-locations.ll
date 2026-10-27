; RUN: llc -O0 -verify-machineinstrs < %s | FileCheck %s --check-prefix=ASM
; RUN: llc -O0 -filetype=obj < %s -o %t.o
; RUN: llvm-dwarfdump --debug-info %t.o | FileCheck %s --check-prefix=INFO
; RUN: llvm-dwarfdump --verify %t.o | FileCheck %s --check-prefix=VERIFY

; Variable locations in a function that realigns its stack.  The locals are
; addressed from the base pointer R7 (R15 rounded down to 32), so they are
; described as DW_OP_breg7; the stack-passed argument stays R15-relative
; (DW_OP_fbreg against DW_AT_frame_base = R15).
;
; Generated from (clang -O0 -g):
;   void use(void *);
;   long realigned(long a, long b, long c, long d, long e) {
;     _Alignas(32) char buf[32];
;     long x = a + e;
;     use(buf);
;     use(&x);
;     return x;
;   }

; ASM-LABEL: realigned:
; ASM:       andi r7, r15, -32
; ASM:       stidx64 r0, r7, -16
; ASM:       ldidx64 {{r[0-9]+}}, r15, 40
; ASM:       stidx64 {{r[0-9]+}}, r7, -104
; ASM:       addi r0, r7, -96
; ASM:       addi r0, r7, -104

; INFO:      DW_TAG_subprogram
; INFO:        DW_AT_frame_base (DW_OP_reg15 R15)
; INFO:        DW_AT_name ("realigned")
; INFO:        DW_TAG_formal_parameter
; INFO-NEXT:     DW_AT_location (DW_OP_breg7 R7-16)
; INFO-NEXT:     DW_AT_name ("a")
; INFO:          DW_AT_name ("d")
; INFO:        DW_TAG_formal_parameter
; INFO-NEXT:     DW_AT_location (DW_OP_fbreg +40)
; INFO-NEXT:     DW_AT_name ("e")
; INFO:        DW_TAG_variable
; INFO-NEXT:     DW_AT_location (DW_OP_breg7 R7-96)
; INFO-NEXT:     DW_AT_name ("buf")
; INFO:        DW_TAG_variable
; INFO-NEXT:     DW_AT_location (DW_OP_breg7 R7-104)
; INFO-NEXT:     DW_AT_name ("x")

; VERIFY: No errors.

target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64"
target triple = "klausscpu-unknown-unknown-elf"

; Function Attrs: noinline nounwind optnone
define dso_local i64 @realigned(i64 noundef %a, i64 noundef %b, i64 noundef %c, i64 noundef %d, i64 noundef %e) #0 !dbg !5 {
entry:
  %a.addr = alloca i64, align 8
  %b.addr = alloca i64, align 8
  %c.addr = alloca i64, align 8
  %d.addr = alloca i64, align 8
  %e.addr = alloca i64, align 8
  %buf = alloca [32 x i8], align 32
  %x = alloca i64, align 8
  store i64 %a, ptr %a.addr, align 8
    #dbg_declare(ptr %a.addr, !10, !DIExpression(), !11)
  store i64 %b, ptr %b.addr, align 8
    #dbg_declare(ptr %b.addr, !12, !DIExpression(), !11)
  store i64 %c, ptr %c.addr, align 8
    #dbg_declare(ptr %c.addr, !13, !DIExpression(), !11)
  store i64 %d, ptr %d.addr, align 8
    #dbg_declare(ptr %d.addr, !14, !DIExpression(), !11)
  store i64 %e, ptr %e.addr, align 8
    #dbg_declare(ptr %e.addr, !15, !DIExpression(), !11)
    #dbg_declare(ptr %buf, !16, !DIExpression(), !21)
    #dbg_declare(ptr %x, !22, !DIExpression(), !23)
  %0 = load i64, ptr %a.addr, align 8, !dbg !23
  %1 = load i64, ptr %e.addr, align 8, !dbg !23
  %add = add nsw i64 %0, %1, !dbg !23
  store i64 %add, ptr %x, align 8, !dbg !23
  %arraydecay = getelementptr inbounds [32 x i8], ptr %buf, i64 0, i64 0, !dbg !24
  call void @use(ptr noundef %arraydecay) #2, !dbg !24
  call void @use(ptr noundef %x) #2, !dbg !25
  %2 = load i64, ptr %x, align 8, !dbg !26
  ret i64 %2, !dbg !26
}

declare dso_local void @use(ptr noundef) #1

attributes #0 = { noinline nounwind optnone "frame-pointer"="all" }
attributes #1 = { "frame-pointer"="all" }
attributes #2 = { nobuiltin }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3, !4}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, splitDebugInlining: false, nameTableKind: None)
!1 = !DIFile(filename: "dbg2.c", directory: "/tmp")
!2 = !{i32 7, !"Dwarf Version", i32 5}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !{i32 7, !"frame-pointer", i32 2}
!5 = distinct !DISubprogram(name: "realigned", scope: !1, file: !1, line: 2, type: !6, scopeLine: 2, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !9)
!6 = !DISubroutineType(types: !7)
!7 = !{!8, !8, !8, !8, !8, !8}
!8 = !DIBasicType(name: "long", size: 64, encoding: DW_ATE_signed)
!9 = !{}
!10 = !DILocalVariable(name: "a", arg: 1, scope: !5, file: !1, line: 2, type: !8)
!11 = !DILocation(line: 2, scope: !5)
!12 = !DILocalVariable(name: "b", arg: 2, scope: !5, file: !1, line: 2, type: !8)
!13 = !DILocalVariable(name: "c", arg: 3, scope: !5, file: !1, line: 2, type: !8)
!14 = !DILocalVariable(name: "d", arg: 4, scope: !5, file: !1, line: 2, type: !8)
!15 = !DILocalVariable(name: "e", arg: 5, scope: !5, file: !1, line: 2, type: !8)
!16 = !DILocalVariable(name: "buf", scope: !5, file: !1, line: 3, type: !17, align: 256)
!17 = !DICompositeType(tag: DW_TAG_array_type, baseType: !18, size: 256, elements: !19)
!18 = !DIBasicType(name: "char", size: 8, encoding: DW_ATE_signed_char)
!19 = !{!20}
!20 = !DISubrange(count: 32)
!21 = !DILocation(line: 3, scope: !5)
!22 = !DILocalVariable(name: "x", scope: !5, file: !1, line: 4, type: !8)
!23 = !DILocation(line: 4, scope: !5)
!24 = !DILocation(line: 5, scope: !5)
!25 = !DILocation(line: 6, scope: !5)
!26 = !DILocation(line: 7, scope: !5)
