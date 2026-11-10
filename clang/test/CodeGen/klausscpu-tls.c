// REQUIRES: klausscpu-registered-target
// RUN: %clang_cc1 -triple klausscpu-unknown-linux-musl -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple klausscpu-unknown-linux-musl -S -O2 -o - %s \
// RUN:   | FileCheck %s --check-prefix=ASM
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -fsyntax-only -verify=baremetal %s

// __thread / _Thread_local: Linux user programs (static executables, TLS
// local exec off TP).  Bare metal sets up no thread pointer, so it has none.

// CHECK: @counter = dso_local thread_local global i32 42
// CHECK: @buf = dso_local thread_local global [16 x i64] zeroinitializer
// CHECK: @ext = external thread_local global i64
__thread int counter = 42; // baremetal-error {{thread-local storage is not supported for the current target}}
_Thread_local long buf[16]; // baremetal-error {{thread-local storage is not supported for the current target}}
extern _Thread_local long ext; // baremetal-error {{thread-local storage is not supported for the current target}}

// ASM-LABEL: bump:
// ASM:         mfsr [[TP:r[0-9]+]], tp
// ASM:         ldidx32 {{r[0-9]+}}, [[TP]], %tprel(counter)
// ASM:         stidx32 {{r[0-9]+}}, [[TP]], %tprel(counter)
int bump(void) { return ++counter; }

// ASM-LABEL: get:
// ASM:         mfsr [[TP:r[0-9]+]], tp
// ASM:         ldidx64 {{r[0-9]+}}, [[TP]], %tprel(buf+24)
// ASM:         ldidx64 {{r[0-9]+}}, [[TP]], %tprel(ext)
long get(void) { return buf[3] + ext; }

// ASM-LABEL: addr:
// ASM:         mfsr [[TP:r[0-9]+]], tp
// ASM-NEXT:    addi {{r[0-9]+}}, [[TP]], %tprel(buf)
long *addr(void) { return buf; }
