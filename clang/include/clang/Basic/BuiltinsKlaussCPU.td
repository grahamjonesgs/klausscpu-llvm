//===--- BuiltinsKlaussCPU.td - KlaussCPU Builtin function database -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

include "clang/Basic/BuiltinsBase.td"

// DI / EI (ISA_ENCODING_V2_MAP §8.2).  __builtin_klausscpu_di() disables
// interrupts and returns the old enable bit (0 or 1);
// __builtin_klausscpu_ei(k) sets the enable bit to k & 1, so
//   unsigned k = __builtin_klausscpu_di(); ...; __builtin_klausscpu_ei(k);
// restores the state it found.  Both are compiler barriers for memory (the
// llvm.klausscpu.di / .ei intrinsics read and write memory).  Not atomic
// against the second (AMP) core.
def DI : TargetBuiltin {
  let Spellings = ["__builtin_klausscpu_di"];
  let Prototype = "unsigned int()";
}

def EI : TargetBuiltin {
  let Spellings = ["__builtin_klausscpu_ei"];
  let Prototype = "void(unsigned int)";
}
