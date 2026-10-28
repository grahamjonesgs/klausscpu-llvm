// RUN: %clang_cc1 -triple klausscpu-unknown-elf -E -dM %s -o - | FileCheck %s

// __klausscpu_abi__ is the calling convention's version (KlaussCPUABI.h):
// 2 since i32 values are sign-extended, aggregates of up to 16 bytes are
// passed in registers and only variadic calls reserve a home area.

// CHECK: #define __KlaussCPU__ 1
// CHECK: #define __klausscpu__ 1
// CHECK: #define __klausscpu_abi__ 2
