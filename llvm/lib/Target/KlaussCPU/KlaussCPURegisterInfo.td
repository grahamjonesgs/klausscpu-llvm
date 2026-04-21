//===-- KlaussCPURegisterInfo.td - KlaussCPU Register Definitions ----------===//
//
// 16 general-purpose 64-bit registers, R0–R15, assembled as A–P (uppercase).
// Hardware encoding = register number (4 bits).
//
// Stack pointer (SP) is a separate 32-bit hardware resource accessed only via
// GETSP / SETSP / ADDSP / PUSH / POP.  It is defined here as a standalone
// register outside the GPR class so LLVM can reference it symbolically in
// frame lowering without ever allocating it.
//
// Calling convention (observed from rcc / klausscc):
//   arg 0–3    : A B C D  (R0–R3)   caller-saved
//   arg 4+     : [CallerSP+32+n*8]  → [P+48+n*8] inside callee
//   return val : M        (R12)     caller-saved
//   callee-saved: E F G H (R4–R7)  + P (R15 / frame pointer)
//   temporaries : I J K L N O      (R8–R11, R13–R14)  caller-saved
//
// Frame pointer convention:
//   LLVM always emits a frame pointer (P = R15).  All frame-slot accesses use
//   P as the base register (LDIDX64 / STIDX64 rd, P, offset).  SP is only
//   touched by the prologue (GETSP P; ADDSP -N) and epilogue (SETSP P; POP P).

class KlaussCPUReg<bits<4> Enc, string n, list<string> alt = []>
    : Register<n, alt> {
  let HWEncoding{3-0} = Enc;
  let Namespace = "KlaussCPU";
}

let Namespace = "KlaussCPU" in {

  // ---- General-purpose 64-bit registers ----
  // DwarfRegNum columns 0–15 match register number.

  // Caller-saved: argument registers
  def R0  : KlaussCPUReg<0,  "r0",  ["A"]>, DwarfRegNum<[0]>;    // A — arg0
  def R1  : KlaussCPUReg<1,  "r1",  ["B"]>, DwarfRegNum<[1]>;    // B — arg1
  def R2  : KlaussCPUReg<2,  "r2",  ["C"]>, DwarfRegNum<[2]>;    // C — arg2
  def R3  : KlaussCPUReg<3,  "r3",  ["D"]>, DwarfRegNum<[3]>;    // D — arg3

  // Callee-saved
  def R4  : KlaussCPUReg<4,  "r4",  ["E"]>, DwarfRegNum<[4]>;    // E
  def R5  : KlaussCPUReg<5,  "r5",  ["F"]>, DwarfRegNum<[5]>;    // F
  def R6  : KlaussCPUReg<6,  "r6",  ["G"]>, DwarfRegNum<[6]>;    // G
  def R7  : KlaussCPUReg<7,  "r7",  ["H"]>, DwarfRegNum<[7]>;    // H

  // Caller-saved temporaries (not used by rcc as arg/return regs)
  def R8  : KlaussCPUReg<8,  "r8",  ["I"]>, DwarfRegNum<[8]>;    // I
  def R9  : KlaussCPUReg<9,  "r9",  ["J"]>, DwarfRegNum<[9]>;    // J
  def R10 : KlaussCPUReg<10, "r10", ["K"]>, DwarfRegNum<[10]>;   // K
  def R11 : KlaussCPUReg<11, "r11", ["L"]>, DwarfRegNum<[11]>;   // L

  // Caller-saved: return value and temporaries
  def R12 : KlaussCPUReg<12, "r12", ["M"]>, DwarfRegNum<[12]>;   // M — return value
  def R13 : KlaussCPUReg<13, "r13", ["N"]>, DwarfRegNum<[13]>;   // N
  def R14 : KlaussCPUReg<14, "r14", ["O"]>, DwarfRegNum<[14]>;   // O

  // Frame pointer (callee-saved; reserved when frame pointer is active)
  def R15 : KlaussCPUReg<15, "r15", ["P"]>, DwarfRegNum<[15]>;   // P — frame pointer

  // ---- Stack pointer: separate 32-bit hardware register ----
  // Not a GPR; never an operand to ALU or load/store instructions.
  // Referenced by frame lowering via getStackPointerRegisterToSaveRestore().
  def SP  : Register<"sp">, DwarfRegNum<[16]> {
    let Namespace = "KlaussCPU";
    let HWEncoding{3-0} = 0;   // no encoding in data-path instructions
  }

  // ---- Virtual return-address DWARF column ----
  // KlaussCPU has no hardware link register; CALL saves the return address
  // directly to [SP].  This pseudo-register (column 17) exists only for the
  // DWARF CIE return_address_register field — it is never allocated.
  def RA  : Register<"ra">, DwarfRegNum<[17]> {
    let Namespace = "KlaussCPU";
  }

} // end Namespace = "KlaussCPU"

// ---- GPR register class (all 16 GPRs, 64-bit aligned) ----
//
// Allocation order: RA prefers registers listed first.
//   1. Return-value and caller-saved temporaries first — free to clobber.
//   2. Argument registers next — also caller-saved, but used for call args.
//   3. Callee-saved last — RA avoids these to reduce save/restore overhead.
//   4. Frame pointer last — almost always reserved.
//
def GPR : RegisterClass<"KlaussCPU", [i64], 64, (add
  // Caller-saved: return + temporaries
  R12,                        // M  — return value
  R14, R13, R11, R10, R9, R8, // O N L K J I
  // Caller-saved: argument registers
  R0, R1, R2, R3,             // A B C D
  // Callee-saved
  R4, R5, R6, R7,             // E F G H
  // Frame pointer (effectively reserved)
  R15                         // P
)>;
