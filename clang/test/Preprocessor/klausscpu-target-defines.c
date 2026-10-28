// RUN: %clang_cc1 -triple klausscpu-unknown-elf -E -dM %s -o - | FileCheck %s
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -E -dM %s -o - \
// RUN:   | FileCheck %s --check-prefix=ATOMIC
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -fsyntax-only -verify %s

// __klausscpu_abi__ is the calling convention's version (KlaussCPUABI.h):
// 2 since i32 values are sign-extended, aggregates of up to 16 bytes are
// passed in registers and only variadic calls reserve a home area.

// CHECK: #define __KlaussCPU__ 1
// CHECK: #define __klausscpu__ 1
// CHECK: #define __klausscpu_abi__ 2

// Atomics are inline (plain loads and stores, __sync_* calls for the rest)
// up to 64 bits, the same max-atomic-width as the Rust target spec
// (llvm/lib/Target/KlaussCPU/rust, checked against the backend by
// llvm/test/CodeGen/KlaussCPU/rust-target-spec.test); 128 bits is not.

// ATOMIC: #define __CLANG_ATOMIC_LLONG_LOCK_FREE 2
// ATOMIC: #define __CLANG_ATOMIC_POINTER_LOCK_FREE 2

// expected-no-diagnostics
_Static_assert(__atomic_always_lock_free(8, 0), "64-bit atomics are inline");
_Static_assert(!__atomic_always_lock_free(16, 0), "128-bit atomics are not");
