// REQUIRES: klausscpu-registered-target
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -std=gnu17 \
// RUN:   -Wno-deprecated-non-prototype -emit-llvm %s -o - | FileCheck %s

// A call through a declaration without a prototype is made as a variadic
// call when all its arguments are in R0-R3 (the sret pointer counts), so it
// reserves the home area a variadic callee stores them in; with stack
// arguments it stays non-variadic, where a non-variadic callee expects them.

struct pair { long a, b; };
struct big { long a, b, c; };

long f();
struct big g();

// CHECK-LABEL: define{{.*}} i64 @none(
// CHECK:         call i64 (...) @f()
long none(void) { return f(); }

// CHECK-LABEL: define{{.*}} i64 @four_words(
// CHECK:         call i64 (i32, i64, i64, i64, ...) @f(i32 noundef signext 3, i64 noundef 1, i64 noundef 2, i64 noundef 3)
long four_words(void) { return f(3, 1L, 2L, 3L); }

// CHECK-LABEL: define{{.*}} i64 @two_word_aggregate(
// CHECK:         call i64 (i32, [2 x i64], i64, ...) @f(i32 noundef signext 1, [2 x i64] %{{.*}}, i64 noundef 4)
long two_word_aggregate(struct pair p) { return f(1, p, 4L); }

// CHECK-LABEL: define{{.*}} i64 @five_words(
// CHECK:         call i64 @f(i32 noundef signext 4, i64 noundef 1, i64 noundef 2, i64 noundef 3, i64 noundef 4)
long five_words(void) { return f(4, 1L, 2L, 3L, 4L); }

// CHECK-LABEL: define{{.*}} i64 @aggregate_spills(
// CHECK:         call i64 @f(i32 noundef signext 2, i64 noundef 1, [2 x i64] %{{.*}}, [2 x i64] %{{.*}})
long aggregate_spills(struct pair p) { return f(2, 1L, p, p); }

// A large aggregate argument is one word, a pointer to the caller's copy.
// CHECK-LABEL: define{{.*}} i64 @by_reference(
// CHECK:         call i64 (ptr, i64, i64, i64, ...) @f(ptr noundef {{.*}}%byval-temp, i64 noundef 1, i64 noundef 2, i64 noundef 3)
long by_reference(struct big b) { return f(b, 1L, 2L, 3L); }

// The sret pointer takes R0, leaving three words.
// CHECK-LABEL: define{{.*}} void @sret_three(
// CHECK:         call void (ptr, i32, i64, i64, ...) @g(ptr {{.*}}sret(%struct.big) {{.*}}, i32 noundef signext 2, i64 noundef 5, i64 noundef 6)
struct big sret_three(void) { return g(2, 5L, 6L); }

// CHECK-LABEL: define{{.*}} void @sret_four(
// CHECK:         call void @g(ptr {{.*}}sret(%struct.big) {{.*}}, i32 noundef signext 3, i64 noundef 5, i64 noundef 6, i64 noundef 7)
struct big sret_four(void) { return g(3, 5L, 6L, 7L); }

// float is promoted to double, a GPR word.
// CHECK-LABEL: define{{.*}} i64 @promoted(
// CHECK:         call i64 (double, i32, ...) @f(double noundef %{{.*}}, i32 noundef signext %{{.*}})
long promoted(float x, short s) { return f(x, s); }
