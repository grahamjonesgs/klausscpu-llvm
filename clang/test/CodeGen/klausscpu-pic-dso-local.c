// Outside Linux KlaussCPU has no shared objects, so nothing can preempt a
// definition: under -fPIC as under -fPIE, clang marks every definition
// dso_local (which lets GlobalMerge merge external globals), a weak one too,
// as for -fPIE (GlobalMerge leaves weak globals alone).  A declaration is
// not.  On Linux (ABI.md "Dynamic linking") the ELF rules hold: -fPIC makes
// default-visibility variable definitions preemptible (a function definition
// stays dso_local, reached through its local alias, as clang does on every
// ELF target without -fsemantic-interposition), -fPIE keeps them local, and
// hidden, protected and static symbols are always local.
// REQUIRES: klausscpu-registered-target
// RUN: %clang_cc1 -triple klausscpu -pic-level 2 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple klausscpu -pic-level 2 -pic-is-pie -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -pic-level 2 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple klausscpu-unknown-linux-musl -pic-level 2 -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=LINUX-PIC
// RUN: %clang_cc1 -triple klausscpu-unknown-linux-musl -pic-level 2 -pic-is-pie -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=LINUX-PIE

// CHECK: @defined = dso_local global i64 0
// LINUX-PIC: @defined = global i64 0
// LINUX-PIE: @defined = dso_local global i64 0
long defined;
// CHECK: @weakdef = weak dso_local global i64 0
// LINUX-PIC: @weakdef = weak global i64 0
// LINUX-PIE: @weakdef = weak dso_local global i64 0
__attribute__((weak)) long weakdef;
// CHECK: @hiddendef = hidden global i64 0
// LINUX-PIC: @hiddendef = hidden global i64 0
// LINUX-PIE: @hiddendef = hidden global i64 0
__attribute__((visibility("hidden"))) long hiddendef;
// CHECK: @protdef = protected global i64 0
// LINUX-PIC: @protdef = protected global i64 0
// LINUX-PIE: @protdef = protected global i64 0
__attribute__((visibility("protected"))) long protdef;
// CHECK: @staticdef = internal global i64 0
// LINUX-PIC: @staticdef = internal global i64 0
// LINUX-PIE: @staticdef = internal global i64 0
static long staticdef;
// CHECK: @declared = external global i64
// LINUX-PIC: @declared = external global i64
// LINUX-PIE: @declared = external global i64
extern long declared;
// CHECK: @hiddendecl = external hidden global i64
// LINUX-PIC: @hiddendecl = external hidden global i64
// LINUX-PIE: @hiddendecl = external hidden global i64
extern __attribute__((visibility("hidden"))) long hiddendecl;
long ext_fn(void);

// CHECK: define dso_local i64 @use()
// LINUX-PIC: define dso_local i64 @use()
// LINUX-PIE: define dso_local i64 @use()
long use(void) {
  return defined + weakdef + hiddendef + protdef + staticdef + declared +
         hiddendecl + ext_fn();
}

// CHECK: declare i64 @ext_fn()
// LINUX-PIC: declare i64 @ext_fn()
// LINUX-PIE: declare i64 @ext_fn()
