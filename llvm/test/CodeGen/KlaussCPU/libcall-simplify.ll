; RUN: opt -mtriple=klausscpu -passes=instcombine -S < %s | FileCheck %s

; TargetLibraryInfo: on KlaussCPU (stdio on a UART that every character waits
; for) printf / fprintf are not turned into puts, putchar, fputc, fputs or
; fwrite, which would link another C library function for no gain.  Other
; library calls are still simplified (strlen of a constant).

@hello = private constant [7 x i8] c"hello\0A\00"
@pc = private constant [3 x i8] c"%c\00"
@lit = private constant [4 x i8] c"abc\00"

declare i32 @printf(ptr, ...)
declare i32 @fprintf(ptr, ptr, ...)
declare i64 @strlen(ptr)

; CHECK-LABEL: @f(
; CHECK-NEXT:    call i32 (ptr, ...) @printf(ptr noundef nonnull dereferenceable(1) @hello)
; CHECK-NEXT:    call i32 (ptr, ...) @printf(ptr noundef nonnull dereferenceable(1) @pc, i32 %c)
; CHECK-NEXT:    call i32 (ptr, ptr, ...) @fprintf(ptr %f, ptr nonnull @lit)
; CHECK-NEXT:    ret i64 3
define i64 @f(ptr %f, i32 %c) {
  call i32 (ptr, ...) @printf(ptr @hello)
  call i32 (ptr, ...) @printf(ptr @pc, i32 %c)
  call i32 (ptr, ptr, ...) @fprintf(ptr %f, ptr @lit)
  %n = call i64 @strlen(ptr @lit)
  ret i64 %n
}
