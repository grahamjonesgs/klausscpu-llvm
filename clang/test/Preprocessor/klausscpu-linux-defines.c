// klausscpu-*-linux-*: the Linux OS macros on top of the KlaussCPU ones.
// RUN: %clang_cc1 -triple klausscpu-unknown-linux-musl -E -dM %s -o - | FileCheck %s
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -E -dM %s -o - \
// RUN:   | FileCheck %s --check-prefix=BARE

// CHECK: #define __ELF__ 1
// CHECK: #define __klausscpu__ 1
// CHECK: #define __linux__ 1
// CHECK: #define __unix__ 1

// BARE-NOT: #define __linux__
// BARE-NOT: #define __unix__
