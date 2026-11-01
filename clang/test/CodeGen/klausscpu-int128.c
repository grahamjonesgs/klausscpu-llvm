// REQUIRES: klausscpu-registered-target
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -emit-llvm -O0 %s -o - | FileCheck %s
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -fsyntax-only -verify %s

// ABI version 3: __int128 is 8-aligned, matching the 8-byte stack, malloc's
// guarantee and __BIGGEST_ALIGNMENT__ (the DataLayout has no i128 entry).
// A struct holding one is laid out on 8 bytes, and a local one no longer
// forces the frame to be realigned (the backend test i128-align.ll).

// expected-no-diagnostics

// CHECK: target datalayout = "e-m:e-p:64:64-i64:64-n32:64"

_Static_assert(_Alignof(__int128) == 8, "");
_Static_assert(_Alignof(unsigned __int128) == 8, "");
_Static_assert(__alignof__(__int128) == 8, "");
_Static_assert(sizeof(__int128) == 16, "");
_Static_assert(__BIGGEST_ALIGNMENT__ == 8, "");

struct A { char c; __int128 v; };
struct B { __int128 v; char c; };
union C { char c; unsigned __int128 v; };
_Static_assert(__builtin_offsetof(struct A, v) == 8, "");
_Static_assert(sizeof(struct A) == 24 && _Alignof(struct A) == 8, "");
_Static_assert(sizeof(struct B) == 24 && _Alignof(struct B) == 8, "");
_Static_assert(sizeof(union C) == 16 && _Alignof(union C) == 8, "");

// An explicit alignment is still honoured.
typedef __int128 __attribute__((aligned(16))) aligned_i128;
_Static_assert(_Alignof(aligned_i128) == 16, "");

// CHECK-LABEL: define{{.*}} void @local(
// CHECK:         %x = alloca i128, align 8
// CHECK:         store i128 {{.*}}, ptr %x, align 8
void use(__int128 *);
void local(void) {
  __int128 x = 1;
  use(&x);
}

// CHECK-LABEL: define{{.*}} i128 @deref(
// CHECK:         load i128, ptr {{.*}}, align 8
__int128 deref(__int128 *p) { return *p; }

// A field of a packed struct stays unaligned.
struct __attribute__((packed)) P { char c; __int128 v; };
// CHECK-LABEL: define{{.*}} i128 @packed(
// CHECK:         load i128, ptr {{.*}}, align 1
__int128 packed(struct P *p) { return p->v; }
