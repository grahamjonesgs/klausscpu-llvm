// REQUIRES: klausscpu-registered-target
// RUN: %clang_cc1 -triple klausscpu-unknown-linux-musl -mrelocation-model static \
// RUN:   -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple klausscpu-unknown-linux-musl -mrelocation-model static \
// RUN:   -S -O2 -o - %s | FileCheck %s --check-prefix=ASM
// RUN: %clang_cc1 -triple klausscpu-unknown-linux-musl -pic-level 2 -pic-is-pie \
// RUN:   -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple klausscpu-unknown-linux-musl -pic-level 2 -pic-is-pie \
// RUN:   -S -O2 -o - %s | FileCheck %s --check-prefix=PIE
// RUN: %clang_cc1 -triple klausscpu-unknown-linux-musl -pic-level 2 \
// RUN:   -emit-llvm -o - %s | FileCheck %s --check-prefix=PIC
// RUN: %clang_cc1 -triple klausscpu-unknown-linux-musl -pic-level 2 \
// RUN:   -S -O2 -o - %s | FileCheck %s --check-prefix=PIC-ASM
// RUN: %clang_cc1 -triple klausscpu-unknown-elf -fsyntax-only -verify=baremetal %s

// __thread / _Thread_local: Linux user programs, TLS off TP (ABI.md).  Code
// that is not PIC uses local exec throughout; -fPIE local exec for its own
// variables and initial exec for others; -fPIC general dynamic.  Bare metal
// sets up no thread pointer, so it has none.

// CHECK: @counter = dso_local thread_local global i32 42
// CHECK: @buf = dso_local thread_local global [16 x i64] zeroinitializer
// CHECK: @ext = external thread_local global i64
// PIC: @counter = thread_local global i32 42
// PIC: @buf = thread_local global [16 x i64] zeroinitializer
// PIC: @ext = external thread_local global i64
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

// PIE-LABEL: bump:
// PIE:         mfsr [[TP:r[0-9]+]], tp
// PIE:         ldidx32 {{r[0-9]+}}, [[TP]], %tprel(counter)
// PIE-LABEL: get:
// PIE:         leapc [[GOT:r[0-9]+]], %gottprel(ext)
// PIE:         ldidx64 [[OFF:r[0-9]+]], [[GOT]], 0
// PIE:         ldidx64 {{r[0-9]+}}, [[TP:r[0-9]+]], %tprel(buf+24)
// PIE:         ldidx64r {{r[0-9]+}}, [[TP]], [[OFF]]

// PIC-ASM-LABEL: bump:
// PIC-ASM:         leapc r0, %tlsgd(counter)
// PIC-ASM-NEXT:    callrel __tls_get_addr
// PIC-ASM-LABEL: get:
// PIC-ASM:         leapc r0, %tlsgd(buf)
// PIC-ASM:         callrel __tls_get_addr
// PIC-ASM:         ldidx64 {{r[0-9]+}}, r12, 24
// PIC-ASM:         leapc r0, %tlsgd(ext)
// PIC-ASM-NEXT:    callrel __tls_get_addr
// PIC-ASM-LABEL: addr:
// PIC-ASM:         leapc r0, %tlsgd(buf)
// PIC-ASM-NEXT:    callrel __tls_get_addr
