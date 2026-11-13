// RUN: %clang_cc1 -triple klausscpu-unknown-elf -E -dM %s -o - | FileCheck %s
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -E -dM %s -o - \
// RUN:   | FileCheck %s --check-prefix=ATOMIC
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -E -dM %s -o - \
// RUN:   | FileCheck %s --check-prefix=SYNC
// RUN: %clang_cc1 -triple klausscpu-unknown-linux-musl -E -dM %s -o - \
// RUN:   | FileCheck %s --check-prefix=SYNC
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -fsyntax-only -verify %s

// __klausscpu_abi__ is the calling convention's version (KlaussCPUABI.h):
// 3 since __int128 is 8-aligned (2: i32 values are sign-extended, aggregates
// of up to 16 bytes are passed in registers and only variadic calls reserve a
// home area).

// CHECK: #define __KlaussCPU__ 1
// CHECK: #define __klausscpu__ 1
// CHECK: #define __klausscpu_abi__ 3

// Atomics are inline (plain loads and stores, __sync_* calls for the rest)
// up to 64 bits, the same max-atomic-width as the Rust target spec
// (llvm/lib/Target/KlaussCPU/rust, checked against the backend by
// llvm/test/CodeGen/KlaussCPU/rust-target-spec.test); 128 bits is not.

// ATOMIC: #define __CLANG_ATOMIC_LLONG_LOCK_FREE 2
// ATOMIC: #define __CLANG_ATOMIC_POINTER_LOCK_FREE 2

// GCC's macros say the same, for code that checks them (glib): the __sync
// builtins work lock-free for 1, 2, 4 and 8 bytes (as __sync_*_N calls,
// LR/SC in musl, the runtime's atomic.c on bare metal), not for 16.

// SYNC-NOT: __GCC_HAVE_SYNC_COMPARE_AND_SWAP_16
// SYNC: #define __GCC_HAVE_SYNC_COMPARE_AND_SWAP_1 1
// SYNC: #define __GCC_HAVE_SYNC_COMPARE_AND_SWAP_2 1
// SYNC: #define __GCC_HAVE_SYNC_COMPARE_AND_SWAP_4 1
// SYNC: #define __GCC_HAVE_SYNC_COMPARE_AND_SWAP_8 1
// SYNC-NOT: __GCC_HAVE_SYNC_COMPARE_AND_SWAP_16

// expected-no-diagnostics
_Static_assert(__atomic_always_lock_free(8, 0), "64-bit atomics are inline");
_Static_assert(!__atomic_always_lock_free(16, 0), "128-bit atomics are not");
