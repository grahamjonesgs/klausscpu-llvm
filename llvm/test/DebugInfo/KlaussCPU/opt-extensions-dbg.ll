; RUN: llc -mtriple=klausscpu -O1 -verify-machineinstrs -stop-after=klausscpu-opt-ext < %s \
; RUN:   | FileCheck %s
; RUN: llc -mtriple=klausscpu -O1 -filetype=obj < %s -o %t.o
; RUN: llvm-dwarfdump --debug-info %t.o | FileCheck %s --check-prefix=INFO
; RUN: llvm-dwarfdump --verify %t.o | FileCheck %s --check-prefix=VERIFY

; KlaussCPUOptExtensions removes an extension whose readers look only at the
; low bits (rule 2), or makes a PHI carry extended values (rule 3).  Either
; changes register bits that no instruction reads but a debug value may: a
; variable can be wider than the bits its code uses.  The removed
; extension's debug values must describe the source extended, and those of
; other registers whose high bits change must become undef unless the
; variable fits in the low bits.  The pass used to point them all at the
; unextended source, so 'y' showed the raw 64-bit argument.

; The variables, in the MIR's IR module ahead of the functions.
; CHECK-DAG: ![[Y:[0-9]+]] = !DILocalVariable(name: "y"
; CHECK-DAG: ![[WIDE:[0-9]+]] = !DILocalVariable(name: "wide"
; CHECK-DAG: ![[LO:[0-9]+]] = !DILocalVariable(name: "lo"
; CHECK-DAG: ![[IDX32:[0-9]+]] = !DILocalVariable(name: "idx",
; CHECK-DAG: ![[IDX64:[0-9]+]] = !DILocalVariable(name: "idx64"

; Rule 2: %z is stored only as i8/i16, so its ZEXTW goes; y (64-bit) shows
; the zero extension of the low 32 bits of x.
; CHECK-LABEL: name: narrow_store
; CHECK-NOT:   ZEXTW
; CHECK:       DBG_VALUE %{{[0-9]+}}, $noreg, ![[Y]], !DIExpression(DW_OP_LLVM_convert, 32, DW_ATE_unsigned, DW_OP_LLVM_convert, 64, DW_ATE_unsigned, DW_OP_stack_value)
; INFO:        DW_AT_location
; INFO-NEXT:   DW_OP_breg0 R0+0, DW_OP_convert {{.*}} "DW_ATE_unsigned_32", DW_OP_convert {{.*}} "DW_ATE_unsigned_64", DW_OP_stack_value
; INFO-NEXT:   DW_AT_name ("y")
; VERIFY:      No errors.
define void @narrow_store(i64 %x, ptr %p, i1 %c) !dbg !5 {
entry:
  %t = trunc i64 %x to i32
  %z = zext i32 %t to i64
  br i1 %c, label %a, label %b
a:
    #dbg_value(i64 %z, !9, !DIExpression(), !11)
  %t8 = trunc i64 %z to i8
  store i8 %t8, ptr %p, !dbg !11
  ret void
b:
  %t16 = trunc i64 %z to i16
  store i16 %t16, ptr %p, !dbg !11
  ret void
}

; Rule 2 through an add: %s's high bits change too.  'wide' (64-bit) is
; undef; 'lo', a 32-bit piece, keeps %s.
; CHECK-LABEL: name: through_add
; CHECK-NOT:   ZEXTW
; CHECK:       DBG_VALUE $noreg, $noreg, ![[WIDE]], !DIExpression()
; CHECK-NEXT:  DBG_VALUE %[[S:[0-9]+]], $noreg, ![[LO]], !DIExpression(DW_OP_LLVM_fragment, 0, 32)
; CHECK-NEXT:  MEMSET32 killed %[[S]]
define void @through_add(i64 %x, ptr %p, i1 %c) !dbg !20 {
entry:
  %t = trunc i64 %x to i32
  %z = zext i32 %t to i64
  br i1 %c, label %a, label %b
a:
  %s = add i64 %z, 5
    #dbg_value(i64 %s, !21, !DIExpression(), !23)
    #dbg_value(i64 %s, !22, !DIExpression(DW_OP_LLVM_fragment, 0, 32), !23)
  %s32 = trunc i64 %s to i32
  store i32 %s32, ptr %p, !dbg !23
  ret void
b:
  %t16 = trunc i64 %z to i16
  store i16 %t16, ptr %p, !dbg !23
  ret void
}

; Rule 3 (ext-phi.ll): the %idx PHI now carries zero-extended values.  The
; 32-bit 'idx' keeps it; 'idx64' is undef.
; CHECK-LABEL: name: chase
; CHECK:       DBG_VALUE %[[IDX:[0-9]+]], $noreg, ![[IDX32]], !DIExpression()
; CHECK-NEXT:  DBG_VALUE $noreg, $noreg, ![[IDX64]], !DIExpression()
; CHECK-NOT:   ZEXTW %[[IDX]]
define i64 @chase(ptr %b) !dbg !30 {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %idx = phi i32 [ 0, %entry ], [ %t, %loop ]
  %acc = phi i64 [ 0, %entry ], [ %acc.next, %loop ]
    #dbg_value(i32 %idx, !31, !DIExpression(), !33)
    #dbg_value(i32 %idx, !32, !DIExpression(), !33)
  %z = zext i32 %idx to i64
  %p = getelementptr inbounds i64, ptr %b, i64 %z
  %v = load volatile i64, ptr %p, align 8, !dbg !33
  %t = trunc i64 %v to i32
  %tz = zext i32 %t to i64
  %acc.next = add i64 %acc, %tz
  %i.next = add i32 %i, 1
  %c = icmp eq i32 %i.next, 200000
  br i1 %c, label %exit, label %loop

exit:
  ret i64 %acc.next
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!3, !4}
!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: "x", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug, nameTableKind: None)
!1 = !DIFile(filename: "d.c", directory: "/")
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !{i32 7, !"Dwarf Version", i32 5}
!6 = !DISubroutineType(types: !7)
!7 = !{null}
!8 = !DIBasicType(name: "unsigned long", size: 64, encoding: DW_ATE_unsigned)
!10 = !DIBasicType(name: "unsigned int", size: 32, encoding: DW_ATE_unsigned)
!5 = distinct !DISubprogram(name: "narrow_store", scope: !1, file: !1, line: 1, type: !6, unit: !0, spFlags: DISPFlagDefinition | DISPFlagOptimized)
!9 = !DILocalVariable(name: "y", scope: !5, file: !1, line: 2, type: !8)
!11 = !DILocation(line: 2, column: 1, scope: !5)
!20 = distinct !DISubprogram(name: "through_add", scope: !1, file: !1, line: 10, type: !6, unit: !0, spFlags: DISPFlagDefinition | DISPFlagOptimized)
!21 = !DILocalVariable(name: "wide", scope: !20, file: !1, line: 11, type: !8)
!22 = !DILocalVariable(name: "lo", scope: !20, file: !1, line: 11, type: !8)
!23 = !DILocation(line: 11, column: 1, scope: !20)
!30 = distinct !DISubprogram(name: "chase", scope: !1, file: !1, line: 20, type: !6, unit: !0, spFlags: DISPFlagDefinition | DISPFlagOptimized)
!31 = !DILocalVariable(name: "idx", scope: !30, file: !1, line: 21, type: !10)
!32 = !DILocalVariable(name: "idx64", scope: !30, file: !1, line: 21, type: !8)
!33 = !DILocation(line: 21, column: 1, scope: !30)
