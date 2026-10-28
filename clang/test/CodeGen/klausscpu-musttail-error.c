// REQUIRES: klausscpu-registered-target
// RUN: not %clang_cc1 -triple klausscpu-unknown-elf -O2 -debug-info-kind=line-tables-only \
// RUN:   -S -o /dev/null %s 2>&1 | FileCheck %s --implicit-check-not=error: \
// RUN:   --implicit-check-not=PLEASE --implicit-check-not="Stack dump"

// A [[clang::musttail]] return that the KlaussCPU backend cannot make a tail
// call (here: a fifth argument goes on the stack) is an error at the call,
// not a backend crash.  One that can be one compiles.

long g4(long, long, long, long);
long g5(long, long, long, long, long);

long ok(long a, long b, long c, long d) {
  [[clang::musttail]] return g4(a, b, c, d);
}

long bad(long a, long b, long c, long d, long e) {
  // CHECK: klausscpu-musttail-error.c:[[#@LINE+1]]:30: error: musttail call cannot be a tail call on KlaussCPU: it has stack, byval or sret arguments, is variadic, or tail calls are disabled
  [[clang::musttail]] return g5(a, b, c, d, e);
}
