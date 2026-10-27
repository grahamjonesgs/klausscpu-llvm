// REQUIRES: klausscpu-registered-target
// RUN: %clang --target=klausscpu-unknown-elf -O1 -S -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefixes=CHECK,HOSTED
// RUN: %clang --target=klausscpu-unknown-elf -O1 -ffreestanding -S -emit-llvm \
// RUN:   -o - %s | FileCheck %s --check-prefixes=CHECK,FREE

// The driver leaves the environment hosted, so the C library functions are
// builtins: a fixed-size memcpy / memset is an intrinsic the backend expands
// inline.  -ffreestanding (picolibc, compiler-rt, kernels) keeps them calls.
// printf is not turned into puts / putchar on KlaussCPU (TargetLibraryInfo):
// that would link another stdio function for output that waits for the UART.

typedef unsigned long size_t;
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int printf(const char *, ...);

// CHECK-LABEL: @load64(
// HOSTED:        load i64, ptr %p, align 1
// FREE:          call ptr @memcpy(
unsigned long load64(const unsigned char *p) {
  unsigned long w;
  memcpy(&w, p, 8);
  return w;
}

struct B { long a, b, c, d; };

// CHECK-LABEL: @clear(
// HOSTED:        call void @llvm.memset.p0.i64(ptr noundef nonnull align 8 dereferenceable(32) %d, i8 0, i64 32, i1 false)
// FREE:          call ptr @memset(
void clear(struct B *d) { memset(d, 0, sizeof *d); }

// CHECK-LABEL: @hello(
// CHECK:         call i32 (ptr, ...) @printf(
// CHECK-NOT:     @puts
// CHECK-NOT:     @putchar
void hello(int c) {
  printf("hello\n");
  printf("%c", c);
}
