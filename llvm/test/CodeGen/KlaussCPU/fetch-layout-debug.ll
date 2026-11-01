; RUN: llc -march=klausscpu -verify-machineinstrs -O2 < %s | FileCheck %s
;
; Debug instructions never influence KlaussCPUFetchLayout: the loop of
; fetch-layout.ll with dbg.value calls between the instructions it moves gets
; the same instruction order (-g changes nothing).
;
; CHECK:       xorv r13, r2, 305419896
; CHECK:       addi r11, r3, 3
; CHECK:       incr r14, r1
; CHECK:       addi r13, r13, 5
; CHECK:       xorv r10, r14, 878082192
; CHECK:       incr r12, r12
; CHECK:       xorv r11, r11, 1234567

define i64 @f(i64 %n, i64 %a, i64 %b, i64 %c) nounwind !dbg !6 {
entry:
  br label %l
l:
  %i = phi i64 [0, %entry], [%i.next, %l]
  %x = phi i64 [%a, %entry], [%x.next, %l]
  %y = phi i64 [%b, %entry], [%y.next, %l]
  %z = phi i64 [%c, %entry], [%z.next, %l]
  %x1 = add i64 %x, 1
  call void @llvm.dbg.value(metadata i64 %x1, metadata !9, metadata !DIExpression()), !dbg !10
  %y1 = xor i64 %y, 305419896
  call void @llvm.dbg.value(metadata i64 %y1, metadata !11, metadata !DIExpression()), !dbg !10
  %z1 = add i64 %z, 3
  call void @llvm.dbg.value(metadata i64 %z1, metadata !11, metadata !DIExpression()), !dbg !10
  %x2 = xor i64 %x1, 878082192
  call void @llvm.dbg.value(metadata i64 %x2, metadata !12, metadata !DIExpression()), !dbg !10
  %y2 = add i64 %y1, 5
  call void @llvm.dbg.value(metadata i64 %y2, metadata !9, metadata !DIExpression()), !dbg !10
  %z2 = xor i64 %z1, 1234567
  call void @llvm.dbg.value(metadata i64 %z2, metadata !12, metadata !DIExpression()), !dbg !10
  %x.next = add i64 %x2, %y2
  %y.next = add i64 %y2, %z2
  %z.next = add i64 %z2, %x1
  %i.next = add i64 %i, 1
  %cc = icmp ult i64 %i.next, %n
  br i1 %cc, label %l, label %e
e:
  %r = add i64 %x.next, %y.next
  %r2 = add i64 %r, %z.next
  ret i64 %r2
}

declare void @llvm.dbg.value(metadata, metadata, metadata)

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!3}
!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: "t", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "t.c", directory: "/")
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !DISubroutineType(types: !5)
!5 = !{}
!6 = distinct !DISubprogram(name: "f", scope: !1, file: !1, line: 1, type: !4, unit: !0)
!7 = !DIBasicType(name: "long", size: 64, encoding: DW_ATE_signed)
!9 = !DILocalVariable(name: "a", scope: !6, file: !1, line: 2, type: !7)
!10 = !DILocation(line: 2, scope: !6)
!11 = !DILocalVariable(name: "b", scope: !6, file: !1, line: 3, type: !7)
!12 = !DILocalVariable(name: "c", scope: !6, file: !1, line: 4, type: !7)
