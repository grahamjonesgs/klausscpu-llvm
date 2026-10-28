//===-- KlaussCPUABI.h - KlaussCPU ABI and ISA constants --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Facts about KlaussCPU that more than one part of the toolchain needs.  The
/// backend, clang, lld and lldb take them from here instead of repeating the
/// numbers.  The other single sources are TargetDataLayout.cpp (the
/// DataLayout), BinaryFormat/ELF.h (EM_KLAUSSCPU) and
/// BinaryFormat/ELFRelocs/KlaussCPU.def (the relocations).
///
/// Where the backend's TableGen files define the same fact (register names,
/// DWARF numbers, the calling convention's registers, instruction encodings)
/// they stay the source for code generation, and
/// llvm/unittests/Target/KlaussCPU checks that the values here agree.  The
/// copies outside this repository (klausscpu-runtime's loaders, klausscc's
/// debugger) are checked by klausscpu-runtime's check-toolchain-constants.py,
/// and the Rust target spec by test/CodeGen/KlaussCPU/rust-target-spec.test.
///
/// The ISA is defined in the hardware repository (ISA_ENCODING_V2_MAP.md,
/// RTL pipeline_core.sv); the calling convention and frame layout by the
/// backend (KlaussCPUCallingConv.td, KlaussCPUFrameLowering.cpp).
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_TARGETPARSER_KLAUSSCPUABI_H
#define LLVM_TARGETPARSER_KLAUSSCPUABI_H

#include <cstdint>

namespace llvm {
namespace KlaussCPUABI {

//===-- Instruction words -------------------------------------------------===//

/// Instructions are 1-3 little-endian 32-bit words, word aligned.
inline constexpr unsigned MinInstBytes = 4;
inline constexpr unsigned MaxInstBytes = 12;
/// A 2- or 3-word instruction's imm32 (word 1) starts this far into it.  The
/// hardware takes PC-relative displacements from the instruction itself, so
/// an R_KLAUSSCPU_PCREL32 field is S + A - P + Word1Offset.
inline constexpr unsigned Word1Offset = 4;

/// Class B system instructions (encodings in KlaussCPUInstrInfo.td).  NOP
/// pads code (the assembler's alignment fill); TRAP is the debugger's
/// software breakpoint.
inline constexpr uint32_t NopWord = 0x6C000000;
inline constexpr uint32_t HaltWord = 0x6C010000;
inline constexpr uint32_t TrapWord = 0x6C040000;
/// Word 0 of JMPREL (the imm32 displacement follows): lld's call thunk.
inline constexpr uint32_t JmpRelWord = 0xA1000000;
/// An architecturally reserved word: LEN [31:30] = 00 and class [29:26] = 0
/// both trap with ERR_INV_OPCODE (ISA_ENCODING_V2_MAP.md §1).  lld fills the
/// gaps between code sections with it, so a stray jump into padding traps
/// as an invalid opcode, neither a breakpoint (TRAP) nor a clean stop (HALT).
inline constexpr uint32_t InvalidWord = 0x00000000;

//===-- ABI version -------------------------------------------------------===//

/// The calling convention (ABI.md in the backend): 2 is the one with i32
/// arguments and results sign-extended, aggregates of up to 16 bytes in
/// registers and the vararg home area only in variadic calls.  Code built for
/// the earlier convention has 0 here.  Every ELF object carries it in the low
/// byte of e_flags, lld refuses to link code of different versions, and clang
/// predefines __klausscpu_abi__ to it.
inline constexpr unsigned ABIVersion = 2;
inline constexpr unsigned EFlagsABIMask = 0xFF;

//===-- Address space -----------------------------------------------------===//

/// The PC, the SP and every effective address are 32 bits (ELFCLASS32),
/// although GPRs and C pointers are 64 bits.
inline constexpr unsigned AddressBits = 32;
inline constexpr uint64_t MaxAddress = 0xFFFFFFFF;

//===-- Registers ---------------------------------------------------------===//

/// R0-R15.  A GPR's hardware number is also its DWARF number.
inline constexpr unsigned NumGPRs = 16;
inline constexpr const char *GPRNames[NumGPRs] = {
    "r0", "r1", "r2",  "r3",  "r4",  "r5",  "r6",  "r7",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
/// DWARF numbers past the GPRs: the 32-bit SP, and the return-address column
/// of the CFI, which a debugger reads as the PC.
inline constexpr unsigned DwarfSP = 16;
inline constexpr unsigned DwarfRA = 17;

/// Calling convention (KlaussCPUCallingConv.td, ABI.md), as GPR numbers.
/// Arguments, in order (named or not).
inline constexpr unsigned ArgGPRs[] = {0, 1, 2, 3};
inline constexpr unsigned NumArgGPRs = sizeof(ArgGPRs) / sizeof(ArgGPRs[0]);
/// Return value: bits [63:0], then [127:64] of an i128 or of an aggregate
/// of 9 to 16 bytes.
inline constexpr unsigned RetGPRs[] = {12, 11};
/// An aggregate up to this size is passed in up to two GPRs (or stack
/// slots) and returned in RetGPRs; a larger one is passed by reference to a
/// copy the caller makes, and returned through a hidden pointer argument.
inline constexpr unsigned MaxDirectAggregateSize = 16;
/// Callee-saved registers the register allocator may use (CSR_KlaussCPU).
/// The frame pointer is callee-saved too, by the prologue itself.
inline constexpr unsigned CalleeSavedGPRs[] = {4, 5, 6, 7};
/// The frame pointer, set up in every function.
inline constexpr unsigned FPReg = 15;
/// The base pointer of a function that realigns its stack.
inline constexpr unsigned BPReg = 7;

//===-- Stack and frame layout --------------------------------------------===//
//
//   [entry SP + IncomingArgOffset ..]  stack arguments, SlotSize each
//   [entry SP + RetAddrSize ..]        a variadic call only: the home slots
//                                      of R0-R3 (VarArgHomeSize), so the
//                                      stack arguments start that much higher
//   [entry SP + 0]                     return address (CALL pushes it)
//   [entry SP - SavedFPSize]           the caller's R15 (ENTER pushes it)
//   R15 = entry SP - SavedFPSize
//
// The caller's SP is entry SP + RetAddrSize, so it stores stack arguments
// from its SP + OutgoingArgOffset.

/// SP stays 8-byte aligned; nothing on the stack is aligned beyond that
/// without a base pointer.
inline constexpr unsigned StackAlign = 8;
/// Every argument slot and every PUSH/POP is 8 bytes.
inline constexpr unsigned SlotSize = 8;
/// CALL pre-decrements SP by this much and stores the return address there.
inline constexpr unsigned RetAddrSize = 8;
/// The prologue pushes the caller's R15 below the return address.
inline constexpr unsigned SavedFPSize = 8;
/// The first stack argument of a non-variadic call, from the callee's entry
/// SP and from the caller's SP before the CALL.  A call without stack
/// arguments reserves nothing.
inline constexpr unsigned IncomingArgOffset = RetAddrSize;
inline constexpr unsigned OutgoingArgOffset = 0;
/// A variadic call reserves home slots for all of R0-R3 in front of its
/// stack arguments; the callee stores the unnamed register arguments there,
/// so they are contiguous with the stack arguments and va_list is a pointer.
inline constexpr unsigned VarArgHomeSize = NumArgGPRs * SlotSize;
/// CFA = the caller's SP = entry SP + RetAddrSize = R15 + FPToCFAOffset; the
/// return address is at CFA - RetAddrSize, the caller's R15 at
/// CFA - FPToCFAOffset.
inline constexpr unsigned FPToCFAOffset = RetAddrSize + SavedFPSize;

} // namespace KlaussCPUABI
} // namespace llvm

#endif // LLVM_TARGETPARSER_KLAUSSCPUABI_H
