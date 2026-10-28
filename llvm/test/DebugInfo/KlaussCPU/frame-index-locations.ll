; RUN: llc -O1 -verify-machineinstrs < %s | FileCheck %s --check-prefix=ASM
; RUN: llc -O1 -filetype=obj < %s -o %t.o
; RUN: llvm-dwarfdump --debug-info %t.o | FileCheck %s --check-prefix=INFO
; RUN: llvm-dwarfdump --verify %t.o | FileCheck %s --check-prefix=VERIFY

; Stack-resident variable locations.  DW_AT_frame_base is R15, so every
; DW_OP_fbreg / DW_OP_breg15 offset must be the R15-relative offset that the
; code uses for the same slot (getFrameIndexReference must agree with
; eliminateFrameIndex).  The generic getFrameIndexReference adds the frame size
; (and misses the +8 for fixed objects), which put every stack variable at the
; wrong address in the debugger.
;
; Generated from (clang -O1 -g):
;   void use(long *);
;   long ext(long);
;
;   __attribute__((optnone, noinline))
;   long frame(long a, long b, long c, long d, long e) {
;     long x = a + e;
;     long arr[4];
;     use(&x);
;     use(arr);
;     return x + d;
;   }
;
;   long spill(long a, long b, long c, long d, long e) {
;     long p = ext(a), q = ext(b), r = ext(c), s = ext(d), t = ext(e);
;     long u = ext(p + q);
;     return a + b + c + d + e + p + q + r + s + t + u;
;   }

; The slots the code uses: a register argument stored to its home slot, the
; stack-passed argument (fixed object, R15+16), a local and a local array.
; ASM-LABEL: frame:
; ASM:       stidx64 r0, r15, -8
; ASM:       ldidx64 r14, r15, 16
; ASM:       stidx64 r12, r15, -40
; ASM:       addi r0, r15, -72

; A register argument spilled by the register allocator, and the stack-passed
; argument described by a DBG_VALUE of its fixed slot.
; ASM-LABEL: spill:
; ASM:       #DEBUG_VALUE: spill:e <- [DW_OP_plus_uconst 16] [$r15+0]
; ASM:       stidx64 r0, r15, -40 # 8-byte Folded Spill
; ASM:       #DEBUG_VALUE: spill:a <- [DW_OP_constu 40, DW_OP_minus] [$r15+0]

; INFO:      DW_TAG_subprogram
; INFO:        DW_AT_frame_base (DW_OP_reg15 R15)
; INFO:        DW_AT_name ("frame")
; INFO:        DW_TAG_formal_parameter
; INFO-NEXT:     DW_AT_location (DW_OP_fbreg -8)
; INFO-NEXT:     DW_AT_name ("a")
; INFO:        DW_TAG_formal_parameter
; INFO-NEXT:     DW_AT_location (DW_OP_fbreg -16)
; INFO-NEXT:     DW_AT_name ("b")
; INFO:        DW_TAG_formal_parameter
; INFO-NEXT:     DW_AT_location (DW_OP_fbreg -24)
; INFO-NEXT:     DW_AT_name ("c")
; INFO:        DW_TAG_formal_parameter
; INFO-NEXT:     DW_AT_location (DW_OP_fbreg -32)
; INFO-NEXT:     DW_AT_name ("d")
; INFO:        DW_TAG_formal_parameter
; INFO-NEXT:     DW_AT_location (DW_OP_fbreg +16)
; INFO-NEXT:     DW_AT_name ("e")
; INFO:        DW_TAG_variable
; INFO-NEXT:     DW_AT_location (DW_OP_fbreg -40)
; INFO-NEXT:     DW_AT_name ("x")
; INFO:        DW_TAG_variable
; INFO-NEXT:     DW_AT_location (DW_OP_fbreg -72)
; INFO-NEXT:     DW_AT_name ("arr")

; INFO:      DW_TAG_subprogram
; INFO:        DW_AT_frame_base (DW_OP_reg15 R15)
; INFO:        DW_AT_name ("spill")
; INFO:        DW_TAG_formal_parameter
; INFO-NEXT:     DW_AT_location
; INFO-NEXT:       [{{.*}}): DW_OP_reg0 R0
; INFO-NEXT:       [{{.*}}): DW_OP_breg15 R15-40
; INFO:          DW_AT_name ("a")
; INFO:          DW_AT_location (DW_OP_fbreg +16)
; INFO-NEXT:     DW_AT_name ("e")

; VERIFY: No errors.

target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64"
target triple = "klausscpu-unknown-unknown-elf"

; Function Attrs: noinline nounwind optnone
define dso_local i64 @frame(i64 noundef %a, i64 noundef %b, i64 noundef %c, i64 noundef %d, i64 noundef %e) local_unnamed_addr #0 !dbg !10 {
entry:
  %a.addr = alloca i64, align 8
  %b.addr = alloca i64, align 8
  %c.addr = alloca i64, align 8
  %d.addr = alloca i64, align 8
  %e.addr = alloca i64, align 8
  %x = alloca i64, align 8
  %arr = alloca [4 x i64], align 8
  store i64 %a, ptr %a.addr, align 8
    #dbg_declare(ptr %a.addr, !15, !DIExpression(), !27)
  store i64 %b, ptr %b.addr, align 8
    #dbg_declare(ptr %b.addr, !16, !DIExpression(), !27)
  store i64 %c, ptr %c.addr, align 8
    #dbg_declare(ptr %c.addr, !17, !DIExpression(), !27)
  store i64 %d, ptr %d.addr, align 8
    #dbg_declare(ptr %d.addr, !18, !DIExpression(), !27)
  store i64 %e, ptr %e.addr, align 8
    #dbg_declare(ptr %e.addr, !19, !DIExpression(), !27)
    #dbg_declare(ptr %x, !20, !DIExpression(), !28)
  %0 = load i64, ptr %a.addr, align 8, !dbg !28
  %1 = load i64, ptr %e.addr, align 8, !dbg !28
  %add = add nsw i64 %0, %1, !dbg !29
  store i64 %add, ptr %x, align 8, !dbg !30
    #dbg_declare(ptr %arr, !21, !DIExpression(), !31)
  call void @use(ptr noundef %x) #5, !dbg !32
  %arraydecay = getelementptr inbounds nuw [4 x i64], ptr %arr, i64 0, i64 0, !dbg !33
  call void @use(ptr noundef %arraydecay) #5, !dbg !33
  %2 = load i64, ptr %x, align 8, !dbg !34
  %3 = load i64, ptr %d.addr, align 8, !dbg !34
  %add1 = add nsw i64 %2, %3, !dbg !35
  ret i64 %add1, !dbg !37
}

declare !dbg !38 dso_local void @use(ptr noundef) local_unnamed_addr #2

; Function Attrs: nounwind
define dso_local i64 @spill(i64 noundef %a, i64 noundef %b, i64 noundef %c, i64 noundef %d, i64 noundef %e) local_unnamed_addr #3 !dbg !42 {
entry:
    #dbg_value(i64 %a, !44, !DIExpression(), !55)
    #dbg_value(i64 %b, !45, !DIExpression(), !55)
    #dbg_value(i64 %c, !46, !DIExpression(), !55)
    #dbg_value(i64 %d, !47, !DIExpression(), !55)
    #dbg_value(i64 %e, !48, !DIExpression(), !55)
  %call = tail call i64 @ext(i64 noundef %a) #6, !dbg !56
    #dbg_value(i64 %call, !49, !DIExpression(), !55)
  %call1 = tail call i64 @ext(i64 noundef %b) #6, !dbg !57
    #dbg_value(i64 %call1, !50, !DIExpression(), !55)
  %call2 = tail call i64 @ext(i64 noundef %c) #6, !dbg !58
    #dbg_value(i64 %call2, !51, !DIExpression(), !55)
  %call3 = tail call i64 @ext(i64 noundef %d) #6, !dbg !59
    #dbg_value(i64 %call3, !52, !DIExpression(), !55)
  %call4 = tail call i64 @ext(i64 noundef %e) #6, !dbg !60
    #dbg_value(i64 %call4, !53, !DIExpression(), !55)
  %add = add nsw i64 %call1, %call, !dbg !61
  %call5 = tail call i64 @ext(i64 noundef %add) #6, !dbg !62
    #dbg_value(i64 %call5, !54, !DIExpression(), !55)
  %add6 = add nsw i64 %b, %a, !dbg !63
  %add7 = add nsw i64 %add6, %c, !dbg !63
  %add8 = add nsw i64 %add7, %d, !dbg !63
  %add9 = add nsw i64 %add8, %e, !dbg !63
  %add10 = add nsw i64 %add9, %call, !dbg !63
  %add11 = add nsw i64 %add10, %call1, !dbg !63
  %add12 = add nsw i64 %add11, %call2, !dbg !63
  %add13 = add nsw i64 %add12, %call3, !dbg !63
  %add14 = add nsw i64 %add13, %call4, !dbg !63
  %add15 = add nsw i64 %add14, %call5, !dbg !64
  ret i64 %add15, !dbg !65
}

declare !dbg !66 dso_local i64 @ext(i64 noundef) local_unnamed_addr #2

attributes #0 = { noinline nounwind optnone "frame-pointer"="all" }
attributes #2 = { "frame-pointer"="all" }
attributes #3 = { nounwind "frame-pointer"="all" }
attributes #5 = { nobuiltin }
attributes #6 = { nobuiltin nounwind }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3, !4}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug, splitDebugInlining: false, nameTableKind: None)
!1 = !DIFile(filename: "fi.c", directory: "/tmp")
!2 = !{i32 7, !"Dwarf Version", i32 5}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !{i32 7, !"frame-pointer", i32 2}
!10 = distinct !DISubprogram(name: "frame", scope: !1, file: !1, line: 5, type: !11, scopeLine: 5, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !14)
!11 = !DISubroutineType(types: !12)
!12 = !{!13, !13, !13, !13, !13, !13}
!13 = !DIBasicType(name: "long", size: 64, encoding: DW_ATE_signed)
!14 = !{!15, !16, !17, !18, !19, !20, !21}
!15 = !DILocalVariable(name: "a", arg: 1, scope: !10, file: !1, line: 5, type: !13)
!16 = !DILocalVariable(name: "b", arg: 2, scope: !10, file: !1, line: 5, type: !13)
!17 = !DILocalVariable(name: "c", arg: 3, scope: !10, file: !1, line: 5, type: !13)
!18 = !DILocalVariable(name: "d", arg: 4, scope: !10, file: !1, line: 5, type: !13)
!19 = !DILocalVariable(name: "e", arg: 5, scope: !10, file: !1, line: 5, type: !13)
!20 = !DILocalVariable(name: "x", scope: !10, file: !1, line: 6, type: !13)
!21 = !DILocalVariable(name: "arr", scope: !10, file: !1, line: 7, type: !22)
!22 = !DICompositeType(tag: DW_TAG_array_type, baseType: !13, size: 256, elements: !23)
!23 = !{!24}
!24 = !DISubrange(count: 4)
!27 = !DILocation(line: 5, scope: !10)
!28 = !DILocation(line: 6, scope: !10)
!29 = !DILocation(line: 6, scope: !10)
!30 = !DILocation(line: 6, scope: !10)
!31 = !DILocation(line: 7, scope: !10)
!32 = !DILocation(line: 8, scope: !10)
!33 = !DILocation(line: 9, scope: !10)
!34 = !DILocation(line: 10, scope: !10)
!35 = !DILocation(line: 10, scope: !10)
!36 = !DILocation(line: 11, scope: !10)
!37 = !DILocation(line: 10, scope: !10)
!38 = !DISubprogram(name: "use", scope: !1, file: !1, line: 1, type: !39, flags: DIFlagPrototyped, spFlags: DISPFlagOptimized)
!39 = !DISubroutineType(types: !40)
!40 = !{null, !41}
!41 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !13, size: 64)
!42 = distinct !DISubprogram(name: "spill", scope: !1, file: !1, line: 13, type: !11, scopeLine: 13, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !43)
!43 = !{!44, !45, !46, !47, !48, !49, !50, !51, !52, !53, !54}
!44 = !DILocalVariable(name: "a", arg: 1, scope: !42, file: !1, line: 13, type: !13)
!45 = !DILocalVariable(name: "b", arg: 2, scope: !42, file: !1, line: 13, type: !13)
!46 = !DILocalVariable(name: "c", arg: 3, scope: !42, file: !1, line: 13, type: !13)
!47 = !DILocalVariable(name: "d", arg: 4, scope: !42, file: !1, line: 13, type: !13)
!48 = !DILocalVariable(name: "e", arg: 5, scope: !42, file: !1, line: 13, type: !13)
!49 = !DILocalVariable(name: "p", scope: !42, file: !1, line: 14, type: !13)
!50 = !DILocalVariable(name: "q", scope: !42, file: !1, line: 14, type: !13)
!51 = !DILocalVariable(name: "r", scope: !42, file: !1, line: 14, type: !13)
!52 = !DILocalVariable(name: "s", scope: !42, file: !1, line: 14, type: !13)
!53 = !DILocalVariable(name: "t", scope: !42, file: !1, line: 14, type: !13)
!54 = !DILocalVariable(name: "u", scope: !42, file: !1, line: 15, type: !13)
!55 = !DILocation(line: 0, scope: !42)
!56 = !DILocation(line: 14, scope: !42)
!57 = !DILocation(line: 14, scope: !42)
!58 = !DILocation(line: 14, scope: !42)
!59 = !DILocation(line: 14, scope: !42)
!60 = !DILocation(line: 14, scope: !42)
!61 = !DILocation(line: 15, scope: !42)
!62 = !DILocation(line: 15, scope: !42)
!63 = !DILocation(line: 16, scope: !42)
!64 = !DILocation(line: 16, scope: !42)
!65 = !DILocation(line: 16, scope: !42)
!66 = !DISubprogram(name: "ext", scope: !1, file: !1, line: 2, type: !67, flags: DIFlagPrototyped, spFlags: DISPFlagOptimized)
!67 = !DISubroutineType(types: !68)
!68 = !{!13, !13}
