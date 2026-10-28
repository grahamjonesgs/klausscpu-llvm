// REQUIRES: klausscpu-registered-target
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -emit-llvm %s -o - | FileCheck %s

// The KlaussCPU C ABI (llvm/lib/Target/KlaussCPU/ABI.md): RISC-V LP64 for
// integers with four argument registers.  Integers narrower than 64 bits are
// extended by the producer: every i32 is sign-extended, also an unsigned one,
// narrower ones by their signedness.  Aggregates of up to 16 bytes are passed
// and returned in one or two GPRs, coerced to an integer of their size, i128
// (16-byte alignment) or [2 x i64]; larger ones by reference to a copy the
// caller makes (not byval) and returned through sret.

#include <stdbool.h>

struct E {};
struct S1 { char a; };
struct S3 { char a, b, c; };
struct S8 { int a, b; };
struct S12 { int a, b, c; };
struct S16 { long a, b; };
struct S16A { __int128 a; };
struct S17 { long a, b; char c; };
struct S24 { long a, b, c; };
struct M { char c; short s; int i; };
union U { int i; char c[12]; };
typedef int v4si __attribute__((vector_size(16)));
typedef int v8si __attribute__((vector_size(32)));
enum En { X = 1 };

// CHECK-LABEL: define{{.*}} void @f_ints(i8 noundef signext %a, i8 noundef zeroext %b, i16 noundef signext %c, i16 noundef zeroext %d, i32 noundef signext %e, i32 noundef signext %f, i64 noundef %g, i64 noundef %h, i1 noundef zeroext %i, i32 noundef signext %j, ptr noundef %p)
void f_ints(signed char a, unsigned char b, short c, unsigned short d, int e,
            unsigned f, long g, unsigned long h, bool i, enum En j, char *p) {}

// CHECK-LABEL: define{{.*}} signext i32 @r_int()
int r_int(void) { return 0; }
// CHECK-LABEL: define{{.*}} signext i32 @r_uint()
unsigned r_uint(void) { return 0; }
// CHECK-LABEL: define{{.*}} signext i16 @r_short()
short r_short(void) { return 0; }
// CHECK-LABEL: define{{.*}} zeroext i16 @r_ushort()
unsigned short r_ushort(void) { return 0; }
// CHECK-LABEL: define{{.*}} zeroext i1 @r_bool()
bool r_bool(void) { return 0; }
// CHECK-LABEL: define{{.*}} i64 @r_long()
long r_long(void) { return 0; }
// CHECK-LABEL: define{{.*}} i128 @f_i128(i128 noundef %a)
__int128 f_i128(__int128 a) { return a; }

// A zero-size record takes no register.
// CHECK-LABEL: define{{.*}} void @f_empty(i32 noundef signext %b)
void f_empty(struct E a, int b) {}
// CHECK-LABEL: define{{.*}} void @r_empty()
struct E r_empty(void) { struct E e; return e; }

// CHECK-LABEL: define{{.*}} void @f_s1(i8 %a.coerce)
void f_s1(struct S1 a) {}
// CHECK-LABEL: define{{.*}} void @f_s3(i24 %a.coerce)
void f_s3(struct S3 a) {}
// CHECK-LABEL: define{{.*}} void @f_s8(i64 %a.coerce)
void f_s8(struct S8 a) {}
// CHECK-LABEL: define{{.*}} void @f_m(i64 %a.coerce)
void f_m(struct M a) {}
// CHECK-LABEL: define{{.*}} void @f_s12([2 x i64] %a.coerce)
void f_s12(struct S12 a) {}
// CHECK-LABEL: define{{.*}} void @f_s16([2 x i64] %a.coerce)
void f_s16(struct S16 a) {}
// CHECK-LABEL: define{{.*}} void @f_s16a(i128 %a.coerce)
void f_s16a(struct S16A a) {}
// CHECK-LABEL: define{{.*}} void @f_u([2 x i64] %a.coerce)
void f_u(union U a) {}
// CHECK-LABEL: define{{.*}} void @f_cf(i64 noundef %a.coerce)
void f_cf(_Complex float a) {}
// CHECK-LABEL: define{{.*}} void @f_cd([2 x i64] noundef %a.coerce)
void f_cd(_Complex double a) {}
// CHECK-LABEL: define{{.*}} void @f_v4(i128 noundef %a.coerce)
void f_v4(v4si a) {}

// Larger: a pointer to the caller's copy, not byval.
// CHECK-LABEL: define{{.*}} void @f_s17(ptr noundef dead_on_return %a)
void f_s17(struct S17 a) {}
// CHECK-LABEL: define{{.*}} void @f_v8(ptr noundef dead_on_return %0)
void f_v8(v8si a) {}

// CHECK-LABEL: define{{.*}} i24 @r_s3()
struct S3 r_s3(void) { struct S3 s = {0}; return s; }
// CHECK-LABEL: define{{.*}} i64 @r_s8()
struct S8 r_s8(void) { struct S8 s = {0}; return s; }
// CHECK-LABEL: define{{.*}} [2 x i64] @r_s12()
struct S12 r_s12(void) { struct S12 s = {0}; return s; }
// CHECK-LABEL: define{{.*}} [2 x i64] @r_s16()
struct S16 r_s16(void) { struct S16 s = {0}; return s; }
// CHECK-LABEL: define{{.*}} i128 @r_s16a()
struct S16A r_s16a(void) { struct S16A s = {0}; return s; }
// CHECK-LABEL: define{{.*}} [2 x i64] @r_cd()
_Complex double r_cd(void) { return 0; }
// CHECK-LABEL: define{{.*}} void @r_s17(ptr dead_on_unwind noalias writable sret(%struct.S17) align 8 %agg.result)
struct S17 r_s17(void) { struct S17 s = {0}; return s; }

// Unnamed arguments are classified like named ones (the callee stores the
// register ones next to the stack ones); a promoted unsigned short is an
// int, so sign-extended.
int vf(int n, ...);
// CHECK-LABEL: define{{.*}} void @call_vf(
// CHECK:         call signext i32 (i32, ...) @vf(i32 noundef signext 1, [2 x i64] %{{.*}}, ptr noundef dead_on_return %{{.*}}, i32 noundef signext %{{.*}}, i32 noundef signext 3, double noundef 2.000000e+00)
void call_vf(struct S12 s, struct S24 t, int i) {
  vf(1, s, t, i, (unsigned short)3, 2.0f);
}
