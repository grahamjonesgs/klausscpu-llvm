// REQUIRES: klausscpu-registered-target
// RUN: not %clang_cc1 -triple klausscpu-unknown-elf -O2 -debug-info-kind=line-tables-only \
// RUN:   -S -o /dev/null %s 2>&1 | FileCheck %s --implicit-check-not=error: \
// RUN:   --implicit-check-not=PLEASE --implicit-check-not="Stack dump"

// A [[clang::musttail]] return that the KlaussCPU backend cannot make a tail
// call (here: a fifth argument goes on the stack, or an aggregate over 16
// bytes, which clang passes as the address of a copy in the caller's frame)
// is an error at the call, not a backend crash or a callee reading a freed
// frame.  One that can be one compiles.

long g4(long, long, long, long);
long g5(long, long, long, long, long);

long ok(long a, long b, long c, long d) {
  [[clang::musttail]] return g4(a, b, c, d);
}

long bad(long a, long b, long c, long d, long e) {
  // CHECK: klausscpu-musttail-error.c:[[#@LINE+1]]:30: error: musttail call cannot be a tail call on KlaussCPU: it has stack, byval or sret arguments, passes the address of a local (such as an aggregate over 16 bytes), is variadic, or tail calls are disabled
  [[clang::musttail]] return g5(a, b, c, d, e);
}

struct big { long a[6]; };
long gb(struct big);

long badagg(struct big s) {
  // CHECK: klausscpu-musttail-error.c:[[#@LINE+1]]:30: error: musttail call cannot be a tail call on KlaussCPU
  [[clang::musttail]] return gb(s);
}
