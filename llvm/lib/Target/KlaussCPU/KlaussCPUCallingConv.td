//===-- KlaussCPUCallingConv.td - KlaussCPU Calling Convention -----------===//
//
// Calling convention for KlaussCPU (ABI.md has the full description and the
// measurements behind it):
//
//   Arg 0–3   → R0–R3 (i64), caller-saved
//   Args 4+   → stack, 8 bytes each, from [entry SP + 8]; a variadic call
//               first reserves a 32-byte home area for R0–R3 there
//               (LowerCall / LowerFormalArguments), so they start at +40
//   Return    → R12 (M, i64); R12+R11 for an i128 or a 9-16 byte aggregate,
//               caller-saved
//   Callee-saved: R4–R7 (E–H)
//   R15 (P, frame pointer) is saved/restored explicitly by prologue/epilogue.
//
// Sub-word argument/return values (i8, i16, i32) are promoted to i64 before
// register or stack assignment — KlaussCPU has no sub-i64 register class, so
// every value travelling through a register slot must be 64 bits wide.  How
// they are extended is the IR's signext/zeroext attribute: clang sign-extends
// every i32, signed or not (as on RV64), and narrower integers by their
// signedness, so the consumer can rely on the upper bits.
//
//===----------------------------------------------------------------------===//

def CC_KlaussCPU : CallingConv<[
  // Promote i8/i16/i32 to i64 before register/stack assignment.
  CCIfType<[i8, i16, i32], CCPromoteToType<i64>>,
  // First four i64 arguments go into R0–R3, named or not.  An argument of
  // two i64 parts (an i128, a 9-16 byte aggregate) may take R3 and the first
  // stack slot.
  CCIfType<[i64], CCAssignToReg<[R0, R1, R2, R3]>>,
  // Remaining arguments spill to the stack (8-byte slots, 8-byte aligned).
  CCAssignToStack<8, 8>
]>;

def RetCC_KlaussCPU : CallingConv<[
  // Promote sub-word return values to i64 first.
  CCIfType<[i8, i16, i32], CCPromoteToType<i64>>,
  // i64 return values: first in R12 (M), second in R11 (L).
  // i128 is split into two i64s by the type legalizer; R12 holds bits[63:0]
  // and R11 holds bits[127:64].  A 9-16 byte aggregate ([2 x i64]) likewise.
  CCIfType<[i64], CCAssignToReg<[R12, R11]>>
]>;

// Callee-saved registers: E–H (R4–R7).
// R15 is always reserved and never appears here (managed manually).
def CSR_KlaussCPU : CalleeSavedRegs<(add R4, R5, R6, R7)>;

// What a call preserves (its register-mask operand): the callee-saved
// registers, the frame pointer the callee's prologue saves and SP.  Without
// R15 a call would end the range of every debug location based on it.
def CSR_KlaussCPU_Call : CalleeSavedRegs<(add CSR_KlaussCPU, R15, SP)>;
