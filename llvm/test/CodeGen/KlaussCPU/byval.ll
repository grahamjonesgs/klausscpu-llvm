; RUN: llc -march=klausscpu -verify-machineinstrs -O1 < %s | FileCheck %s
;
; byval arguments.  The callee owns the pointee of a byval pointer and may
; modify it, so the caller must pass the address of a private copy.  The
; original object (here a constant in .rodata, a caller-owned pointer, or a
; global) must never be passed directly.  The copy is made before the call
; sequence starts: a large copy is itself a memcpy call, and in a function with
; a VLA the call frame is only allocated (addsp) once the copy is done.

%struct.S = type { i64, i64, i64, i64 }
%struct.L = type { [40 x i64] }

@cs = internal constant %struct.S { i64 1, i64 2, i64 3, i64 4 }
@gl = global %struct.L zeroinitializer

declare void @take(ptr byval(%struct.S) align 8)
declare void @take5(i64, i64, i64, i64, ptr byval(%struct.S) align 8)
declare void @takel(ptr byval(%struct.L) align 8)
declare void @use(ptr)

; Copied to a stack object; R0 gets the copy, not @cs.  The copy reads @cs
; (in whatever order the scheduler picks) and R0 is never overwritten.
define void @pass_const() {
; CHECK-LABEL: pass_const:
; CHECK:         enter 7
; CHECK-NOT:     {{(setr|copy) r0,}}
; CHECK-DAG:     setru {{r([1-9]|1[0-5])}}, cs+24
; CHECK-DAG:     setru {{r([1-9]|1[0-5])}}, cs+16
; CHECK-DAG:     setru {{r([1-9]|1[0-5])}}, cs+8
; CHECK-DAG:     setru {{r([1-9]|1[0-5])}}, cs{{$}}
; CHECK-DAG:     addi r0, r15, -32
; CHECK-DAG:     stidx64 r12, r15, -8
; CHECK-DAG:     stidx64 r12, r15, -16
; CHECK-DAG:     stidx64 r12, r15, -24
; CHECK-DAG:     stidx64 r12, r15, -32
; CHECK-NOT:     {{(setr|copy) r0,}}
; CHECK:         call take
  call void @take(ptr byval(%struct.S) align 8 @cs)
  ret void
}

; byval in a stack argument slot: the copy's address is stored to the slot.
define void @pass_stack_slot(ptr %p) {
; CHECK-LABEL: pass_stack_slot:
; CHECK:         enter 8
; CHECK-DAG:     ldidx64 [[W3:r[0-9]+]], r0, 24
; CHECK-DAG:     ldidx64 [[W0:r[0-9]+]], r0, 0
; CHECK-DAG:     addi [[COPY:r[0-9]+]], r15, -32
; CHECK-DAG:     stidx64 [[W3]], r15, -8
; CHECK-DAG:     stidx64 [[W0]], r15, -32
; CHECK-DAG:     getsp [[SP:r[0-9]+]]
; CHECK:         stidx64 [[COPY]], [[SP]], 24
; CHECK-NEXT:    call take5
  call void @take5(i64 10, i64 20, i64 30, i64 40, ptr byval(%struct.S) align 8 %p)
  ret void
}

; Too large to inline: the copy is a memcpy call made before the call to takel.
define void @pass_large() {
; CHECK-LABEL: pass_large:
; CHECK:         addi [[COPY:r[0-9]+]], r15, -328
; CHECK-DAG:     copy r0, [[COPY]]
; CHECK-DAG:     setru r1, gl
; CHECK-DAG:     setr r2, 320
; CHECK:         call memcpy
; CHECK-NEXT:    copy r0, [[COPY]]
; CHECK-NEXT:    call takel
  call void @takel(ptr byval(%struct.L) align 8 @gl)
  ret void
}

; No reserved call frame: the copy is made before ADJCALLSTACKDOWN's addsp.
define void @pass_vla(i64 %n, ptr %p) {
; CHECK-LABEL: pass_vla:
; CHECK:         call use
; CHECK-NEXT:    addsp 24
; CHECK-DAG:     ldidx64 [[W3:r[0-9]+]], [[P:r[0-9]+]], 24
; CHECK-DAG:     ldidx64 [[W0:r[0-9]+]], [[P]], 0
; CHECK-DAG:     stidx64 [[W3]], r15, -16
; CHECK-DAG:     stidx64 [[W0]], r15, -40
; CHECK:         addsp -32
; CHECK-NEXT:    getsp [[SP:r[0-9]+]]
; CHECK-NEXT:    addi [[COPY:r[0-9]+]], r15, -40
; CHECK:         stidx64 [[COPY]], [[SP]], 24
; CHECK-NEXT:    call take5
; CHECK-NEXT:    addsp 32
  %a = alloca i8, i64 %n, align 8
  call void @use(ptr %a)
  call void @take5(i64 10, i64 20, i64 30, i64 40, ptr byval(%struct.S) align 8 %p)
  ret void
}

; Odd-size, align-1 byval: inlined as byte loads from the original and
; stores into the copy, and R0 gets the copy's frame address.  The copy's
; stores are R15-relative; their offsets, relative to the addi's, prove the
; copy is what is passed.
declare void @take13(ptr byval([13 x i8]) align 1)
declare void @take3(ptr byval({ i8, i8, i8 }) align 1)

define void @pass_odd(ptr %p, ptr %q) {
; CHECK-LABEL: pass_odd:
; CHECK:         stidx8 {{r[0-9]+}}, r15, [[#%d,E13:]]
; CHECK-DAG:     stidx32 {{r[0-9]+}}, r15, [[#%d,E13-4]]
; CHECK-DAG:     addi r0, r15, [[#%d,E13-12]]
; CHECK-NOT:     {{(copy|addi|setr) r0,}}
; CHECK:         stidx64 {{r[0-9]+}}, r15, [[#%d,E13-12]]
; CHECK-NEXT:    call take13
; CHECK:         addi r0, r15, [[#%d,C3:]]
; CHECK-NOT:     {{(copy|addi|setr) r0,}}
; CHECK:         stidx8 {{r[0-9]+}}, r15, [[#%d,C3+2]]
; CHECK-NOT:     {{(copy|addi|setr) r0,}}
; CHECK:         stidx16 {{r[0-9]+}}, r15, [[#%d,C3]]
; CHECK-NEXT:    call take3
  call void @take13(ptr byval([13 x i8]) align 1 %p)
  call void @take3(ptr byval({ i8, i8, i8 }) align 1 %q)
  ret void
}

; Zero-size byval (GNU C `struct E {}`, or `struct { int a[0]; }`, both of
; which clang passes byval): nothing to copy, so the original pointer is
; passed and no stack object is created.
declare void @take_empty(ptr byval({}) align 1)
declare void @take_zla(ptr byval([0 x i32]) align 4)

define void @pass_empty(ptr %p, ptr %q) {
; CHECK-LABEL: pass_empty:
; CHECK:         enter 4
; CHECK-NOT:     addi
; CHECK:         copy r4, r1
; CHECK-NEXT:    call take_empty
; CHECK-NEXT:    copy r0, r4
; CHECK-NEXT:    call take_zla
  call void @take_empty(ptr byval({}) align 1 %p)
  call void @take_zla(ptr byval([0 x i32]) align 4 %q)
  ret void
}

; Callee side: the byval pointer arrives in R0 and is used in place.
define i64 @callee(ptr byval(%struct.S) align 8 %s) {
; CHECK-LABEL: callee:
; CHECK:         setr r12, 7
; CHECK-NEXT:    stidx64 r12, r0, 0
; CHECK-NEXT:    ldidx64 r12, r0, 24
; CHECK-NEXT:    leaveret
  store i64 7, ptr %s
  %v = getelementptr i8, ptr %s, i64 24
  %r = load i64, ptr %v
  ret i64 %r
}
