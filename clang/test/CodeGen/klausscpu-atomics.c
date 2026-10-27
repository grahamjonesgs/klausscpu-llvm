// REQUIRES: klausscpu-registered-target
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -emit-llvm %s -o - | FileCheck %s
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -fgnuc-version=4.2.1 -E -dM %s -o - \
// RUN:   | FileCheck %s --check-prefix=MACROS
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -fgnuc-version=4.2.1 -E -dM %s -o - \
// RUN:   | FileCheck %s --check-prefix=NOSYNC

// Atomics up to 8 bytes are inline (MaxAtomicInlineWidth = 64): clang emits
// IR atomic instructions and the backend makes them plain loads/stores or
// __sync_* libcalls ("forced atomics").  Wider ones are not lock-free; the
// backend's AtomicExpand makes them __atomic_* calls.

// MACROS-DAG: #define __CLANG_ATOMIC_CHAR_LOCK_FREE 2
// MACROS-DAG: #define __CLANG_ATOMIC_LLONG_LOCK_FREE 2
// MACROS-DAG: #define __CLANG_ATOMIC_POINTER_LOCK_FREE 2
// MACROS-DAG: #define __GCC_ATOMIC_CHAR_LOCK_FREE 2
// MACROS-DAG: #define __GCC_ATOMIC_SHORT_LOCK_FREE 2
// MACROS-DAG: #define __GCC_ATOMIC_INT_LOCK_FREE 2
// MACROS-DAG: #define __GCC_ATOMIC_LONG_LOCK_FREE 2
// MACROS-DAG: #define __GCC_ATOMIC_LLONG_LOCK_FREE 2
// MACROS-DAG: #define __GCC_ATOMIC_POINTER_LOCK_FREE 2
// There is no compare-and-swap instruction (the __sync builtins are library
// calls), so, as for RISC-V without Zalrsc, __GCC_HAVE_SYNC_COMPARE_AND_SWAP_N
// stays undefined.
// NOSYNC-NOT: __GCC_HAVE_SYNC_COMPARE_AND_SWAP

// CHECK-LABEL: @load_int(
// CHECK: load atomic i32, ptr %{{.*}} seq_cst, align 4
int load_int(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

// CHECK-LABEL: @store_char(
// CHECK: store atomic i8 %{{.*}}, ptr %{{.*}} release, align 1
void store_char(char *p, char v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

// CHECK-LABEL: @add_short(
// CHECK: atomicrmw add ptr %{{.*}}, i16 %{{.*}} seq_cst, align 2
short add_short(short *p, short v) {
  return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}

// CHECK-LABEL: @cas_long(
// CHECK: cmpxchg ptr %{{.*}}, i64 %{{.*}}, i64 %{{.*}} seq_cst seq_cst, align 8
_Bool cas_long(long *p, long *e, long n) {
  return __atomic_compare_exchange_n(p, e, n, 0, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST);
}

// CHECK-LABEL: @c11_inc(
// CHECK: atomicrmw add ptr @c11, i32 1 seq_cst, align 4
_Atomic int c11;
void c11_inc(void) { c11++; }

// CHECK-LABEL: @wide(
// CHECK: load atomic i128, ptr %{{.*}} seq_cst, align 8
struct S { long a, b; };
struct S wide(struct S *p) {
  struct S r;
  __atomic_load(p, &r, __ATOMIC_SEQ_CST);
  return r;
}
