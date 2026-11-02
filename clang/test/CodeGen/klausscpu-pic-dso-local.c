// KlaussCPU has no shared objects, so nothing can preempt a definition:
// under -fPIC as under -fPIE, clang marks every definition dso_local (which
// lets GlobalMerge merge external globals), a weak one too, as for -fPIE
// (GlobalMerge leaves weak globals alone).  A declaration is not.
// REQUIRES: klausscpu-registered-target
// RUN: %clang_cc1 -triple klausscpu -pic-level 2 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple klausscpu -pic-level 2 -pic-is-pie -emit-llvm -o - %s | FileCheck %s

// CHECK: @defined = dso_local global i64 0
long defined;
// CHECK: @weakdef = weak dso_local global i64 0
__attribute__((weak)) long weakdef;
// CHECK: @declared = external global i64
extern long declared;

// CHECK: define dso_local i64 @use()
long use(void) { return defined + weakdef + declared; }
