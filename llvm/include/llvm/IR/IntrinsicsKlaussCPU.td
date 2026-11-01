//===- IntrinsicsKlaussCPU.td - Defines KlaussCPU intrinsics -*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines all of the KlaussCPU-specific intrinsics.
//
//===----------------------------------------------------------------------===//

let TargetPrefix = "klausscpu" in {  // All intrinsics start with "llvm.klausscpu."
  // DI / EI (ISA_ENCODING_V2_MAP §8.2): the core's interrupt-enable bit.
  // di disables interrupts and returns the old enable bit (0 or 1); ei
  // sets the enable bit to the low bit of its operand, so
  //   k = di(); ...; ei(k);
  // restores the state it found.  Both are i64 because that is the only
  // register type; clang's __builtin_klausscpu_di / _ei take and return
  // unsigned int.
  //
  // Not IntrNoMem and not IntrArgMemOnly: the default is "reads and writes
  // any memory", so the optimizers neither move a memory access across the
  // critical section's edges nor combine two sections, and IntrHasSideEffects
  // keeps them from being deleted or reordered with each other.
  def int_klausscpu_di : Intrinsic<[llvm_i64_ty], [], [IntrHasSideEffects, IntrWillReturn]>;
  def int_klausscpu_ei : Intrinsic<[], [llvm_i64_ty], [IntrHasSideEffects, IntrWillReturn]>;
}
