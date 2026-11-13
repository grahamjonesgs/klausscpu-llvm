// RUN: %clang_cc1 -triple klausscpu-unknown-linux-musl -emit-llvm %s -o - \
// RUN:   | FileCheck %s

// The unwinder enters a landing pad with the exception pointer in R0 and the
// selector in R1 (the backend's getExceptionPointerRegister /
// getExceptionSelectorRegister; ABI.md "Exceptions").  Personality routines
// (libc++abi, libunwind's _Unwind_SetGR callers) name them by their DWARF
// numbers.

// CHECK-LABEL: @exn_reg
// CHECK: ret i32 0
int exn_reg(void) { return __builtin_eh_return_data_regno(0); }

// CHECK-LABEL: @sel_reg
// CHECK: ret i32 1
int sel_reg(void) { return __builtin_eh_return_data_regno(1); }
