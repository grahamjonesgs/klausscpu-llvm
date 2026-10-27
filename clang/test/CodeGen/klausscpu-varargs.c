// REQUIRES: klausscpu-registered-target
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -emit-llvm %s -o - | FileCheck %s

// va_list is a pointer to 8-byte slots (every argument is promoted to i64 or
// split into i64 parts, nothing is aligned beyond 8), so va_arg is a plain
// load and a pointer bump by the slot-rounded size, never realigned; an
// aggregate's slot holds its address.  No IR va_arg instruction.

#include <stdarg.h>

struct S { long a, b, c; };

// CHECK-NOT: va_arg

// CHECK-LABEL: define{{.*}} i32 @va_int(
// CHECK:         %argp.cur = load ptr, ptr %ap.addr, align 8
// CHECK-NEXT:    %argp.next = getelementptr inbounds i8, ptr %argp.cur, i64 8
// CHECK-NEXT:    store ptr %argp.next, ptr %ap.addr, align 8
// CHECK-NEXT:    load i32, ptr %argp.cur, align 8
int va_int(va_list ap) { return va_arg(ap, int); }

// CHECK-LABEL: define{{.*}} i64 @va_long(
// CHECK:         %argp.next = getelementptr inbounds i8, ptr %argp.cur, i64 8
// CHECK:         load i64, ptr %argp.cur, align 8
long va_long(va_list ap) { return va_arg(ap, long); }

// CHECK-LABEL: define{{.*}} double @va_double(
// CHECK:         %argp.next = getelementptr inbounds i8, ptr %argp.cur, i64 8
// CHECK:         load double, ptr %argp.cur, align 8
double va_double(va_list ap) { return va_arg(ap, double); }

// Two slots, not realigned to 16.
// CHECK-LABEL: define{{.*}} i128 @va_i128(
// CHECK-NOT:     and
// CHECK:         %argp.next = getelementptr inbounds i8, ptr %argp.cur, i64 16
// CHECK:         load i128, ptr %argp.cur, align 8
__int128 va_i128(va_list ap) { return va_arg(ap, __int128); }

// CHECK-LABEL: define{{.*}} void @va_struct(
// CHECK:         %argp.next = getelementptr inbounds i8, ptr %argp.cur, i64 8
// CHECK:         [[P:%.*]] = load ptr, ptr %argp.cur, align 8
// CHECK:         call void @llvm.memcpy.p0.p0.i64(ptr align 8 %agg.result, ptr align 8 [[P]], i64 24, i1 false)
struct S va_struct(va_list ap) { return va_arg(ap, struct S); }
