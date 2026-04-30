//===-- KlaussCPUCallingConv.td - KlaussCPU Calling Convention -----------===//
//
// Calling convention for KlaussCPU:
//
//   Arg 0–3   → R0–R3 (i64), caller-saved
//   Args 4+   → stack, 8 bytes each, at [SP+32], [SP+40], … from callee view
//   Return    → R12 (M, i64), caller-saved
//   Callee-saved: R4–R7 (E–H)
//   R15 (P, frame pointer) is saved/restored explicitly by prologue/epilogue.
//
// Sub-word argument/return values (i8, i16, i32) are promoted to i64 before
// register or stack assignment — KlaussCPU has no sub-i64 register class, so
// every value travelling through a register slot must be 64 bits wide.
//
//===----------------------------------------------------------------------===//

def CC_KlaussCPU : CallingConv<[
  // Promote i8/i16/i32 to i64 before register/stack assignment.
  CCIfType<[i8, i16, i32], CCPromoteToType<i64>>,
  // First four i64 arguments go into R0–R3.
  CCIfType<[i64], CCAssignToReg<[R0, R1, R2, R3]>>,
  // Remaining arguments spill to the stack (8-byte slots, 8-byte aligned).
  CCAssignToStack<8, 8>
]>;

def RetCC_KlaussCPU : CallingConv<[
  // Promote sub-word return values to i64 first.
  CCIfType<[i8, i16, i32], CCPromoteToType<i64>>,
  // Single i64 return value goes in R12 (M register).
  CCIfType<[i64], CCAssignToReg<[R12]>>
]>;

// Callee-saved registers: E–H (R4–R7).
// R15 is always reserved and never appears here (managed manually).
def CSR_KlaussCPU : CalleeSavedRegs<(add R4, R5, R6, R7)>;
