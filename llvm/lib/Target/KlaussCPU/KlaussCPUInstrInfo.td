//===-- KlaussCPUInstrInfo.td - KlaussCPU Instruction Definitions ----------===//
//
// ENCODING (ISA v2 + v3; hardware repo klaussCPU/ISA_ENCODING_V2_MAP.md): each
// instruction's `Inst` field is its machine encoding, and
// KlaussCPUMCCodeEmitter emits it through the generated
// getBinaryCodeForInstr.  Every format class takes the instruction's word-0
// template -- the "Template" column of the ISA map: LEN, CLASS and the
// attribute/OP bits, with every operand field zero -- and places its
// operands on top of it:
//
//   word 0 = Inst{31-0}   [31:30]=LEN (01/10/11 = 1/2/3 words)  [29:26]=CLASS
//                         [25:12]=attributes+OP  [11:8]=rd  [7:4]=rs1
//                         [3:0]=rs2   (stores put their data source in rd)
//   word 1 = Inst{63-32}  imm32 / address / branch target   (2 and 3 words)
//   word 2 = Inst{95-64}  high half of an imm64               (3 words)
//
// Words are stored little-endian (Inst{7-0} is the byte at the lowest
// address).  Operand fields the instruction does not use stay 0: the CPU
// matches them strictly and traps otherwise.
//

//===----------------------------------------------------------------------===//
// SDNodes for instruction selection
//===----------------------------------------------------------------------===//

// Function return — emitted by KlaussCPUTargetLowering::LowerReturn.
// SDNPVariadic allows the node to carry register and glue operands.
def KlaussCPURetGlue : SDNode<"KlaussCPUISD::RET_GLUE", SDTNone,
                              [SDNPHasChain, SDNPOptInGlue, SDNPVariadic]>;

// Direct/indirect call — emitted by LowerCall.
// Callee is i64 (address zero-extended into a GPR; encoder uses bits [31:0]).
def SDT_KlaussCPUCall : SDTypeProfile<0, -1, [SDTCisVT<0, i64>]>;
def KlaussCPUCallNode : SDNode<"KlaussCPUISD::CALL", SDT_KlaussCPUCall,
                               [SDNPHasChain, SDNPOptInGlue, SDNPOutGlue,
                                SDNPVariadic]>;


//===----------------------------------------------------------------------===//
// Assembler operand match classes
//
// Every immediate operand names the values its field can hold.  The generated
// matcher rejects any other constant with the class's DiagnosticString, so an
// out-of-range value is an error rather than silently truncated by the
// encoder.  A symbolic expression is accepted only by the classes of operands
// whose EncoderMethod emits a fixup (allowSym).
//===----------------------------------------------------------------------===//

// A constant in [low, high] that is a multiple of scale, or with allowSym any
// non-constant expression (KlaussCPUOperand::isImmRange).
class KCImmAsmOperand<string name, int low, int high, int scale = 1,
                      bit allowSym = 0> : AsmOperandClass {
  let Name             = name;
  let RenderMethod     = "addImmOperands";
  let PredicateMethod  = "isImmRange<" # low # ", " # high # ", " # scale #
                         ", " # !if(allowSym, "true", "false") # ">";
  let DiagnosticType   = "Invalid" # name;
  let DiagnosticString = "immediate must be " #
                         !if(!gt(scale, 1), "a multiple of " # scale # " in",
                             "an integer in") #
                         " the range [" # low # ", " # high # "]" #
                         !if(allowSym, " or a symbol", "");
}

class SImmAsmOperand<int width, bit allowSym = 0, string suffix = "">
    : KCImmAsmOperand<"SImm" # width # suffix,
                      !sub(0, !shl(1, !sub(width, 1))),
                      !sub(!shl(1, !sub(width, 1)), 1), 1, allowSym>;

class UImmAsmOperand<int width, bit allowSym = 0, string suffix = "">
    : KCImmAsmOperand<"UImm" # width # suffix, 0, !sub(!shl(1, width), 1), 1,
                      allowSym>;

// Fields the CPU sign- or zero-extends to 64 bits (ISA_ENCODING_V2_MAP §2,
// SGN), the shift count / bit index N and the ENTER frame size.
def SImm32AsmOperand : SImmAsmOperand<32>;
def UImm32AsmOperand : UImmAsmOperand<32>;
def UImm22AsmOperand : UImmAsmOperand<22>;
def UImm6AsmOperand  : UImmAsmOperand<6>;

// A raw 32-bit word that is not extended (SETR64 / PUSHV64 halves, the
// peripheral immediates): either spelling of the 32-bit pattern.
def Imm32AsmOperand : KCImmAsmOperand<"Imm32", -2147483648, 4294967295>;

// SETR: the CPU sign-extends the imm32.  An unsigned 32-bit constant is also
// accepted as its 32-bit pattern (`setr r0, 0xF00F0000` for an MMIO address
// is sign-extended exactly like the ABS32 address of a symbol there).
def Imm32SymAsmOperand
    : KCImmAsmOperand<"Imm32Sym", -2147483648, 4294967295, 1, 1>;

// Absolute address: a symbol or a 32-bit address (FK_KlaussCPU_ABS32).
def UImm32SymAsmOperand : UImmAsmOperand<32, 1, "Sym">;

// PC-relative target (jmp*rel, callrel, leapc, short and fused branches).  A
// bare number is the byte displacement from the instruction, as the
// disassembler prints it; an operand that names a symbol is the target
// address (KlaussCPUAsmParser::parsePCRelTarget).  Short / fused branches
// store a word displacement, so their constant must be a multiple of 4.
class PCRelAsmOperand<int width, int scale>
    : KCImmAsmOperand<"PCRel" # width, !sub(0, !shl(1, !sub(width, 1))),
                      !sub(!shl(1, !sub(width, 1)), scale), scale, 1> {
  let ParserMethod = "parsePCRelTarget";
}
def PCRel32AsmOperand : PCRelAsmOperand<32, 1>;
def PCRel20AsmOperand : PCRelAsmOperand<20, 4>;
def PCRel15AsmOperand : PCRelAsmOperand<15, 4>;

// The fused-branch condition is PRED | INV << 3 with PRED 0..4; the CPU
// traps on PRED 5..7 (pipeline_core.sv), so those are rejected too.
def FBRCondAsmOperand : AsmOperandClass {
  let Name             = "FBRCond";
  let PredicateMethod  = "isFBRCond";
  let RenderMethod     = "addImmOperands";
  let DiagnosticType   = "InvalidFBRCond";
  let DiagnosticString = "fused-branch condition must be an integer in the "
                         "range [0, 4] or [8, 12]";
}

def SImm4AsmOperand : SImmAsmOperand<4>;

//===----------------------------------------------------------------------===//
// Operand types
//
// Operands that may name a symbol have an EncoderMethod that emits the fixup
// (KlaussCPUMCCodeEmitter); everything else is a plain register or constant.
// The DecoderMethod (KlaussCPUDisassembler) extends the field the way the CPU
// does: a constant operand of a zero-extending op (ISA_ENCODING_V2_MAP §2,
// SGN=0) and an absolute address decode unsigned, everything else signed.
// PC-relative operands decode to the byte displacement from the instruction.
//===----------------------------------------------------------------------===//

def simm32 : Operand<i64> {
  let DecoderMethod    = "decodeSImmOperand<32>";
  let ParserMatchClass = SImm32AsmOperand;
}

def uimm32 : Operand<i64> {
  let DecoderMethod    = "decodeUImmOperand<32>";
  let ParserMatchClass = UImm32AsmOperand;
}

// Raw 32-bit word, printed signed.
def imm32 : Operand<i64> {
  let DecoderMethod    = "decodeSImmOperand<32>";
  let ParserMatchClass = Imm32AsmOperand;
}

// Shift count / bit index in word0[20:15].
def uimm6 : Operand<i64>, ImmLeaf<i64, [{ return isUInt<6>(Imm); }]> {
  let DecoderMethod    = "decodeUImmOperand<6>";
  let ParserMatchClass = UImm6AsmOperand;
}

// ENTER frame size in dwords, word0[21:0].
def uimm22 : Operand<i64> {
  let DecoderMethod    = "decodeUImmOperand<22>";
  let ParserMatchClass = UImm22AsmOperand;
}

// Word-1 immediate that may also be a symbol's address: FK_KlaussCPU_ABS32.
// SETR sign-extends it; MEMREADR / MEMSETR take an unsigned address.
def simm32_abs : Operand<i64> {
  let EncoderMethod    = "getAbs32OpValue";
  let DecoderMethod    = "decodeSImmOperand<32>";
  let ParserMatchClass = Imm32SymAsmOperand;
}

def uimm32_abs : Operand<i64> {
  let EncoderMethod    = "getAbs32OpValue";
  let DecoderMethod    = "decodeUImmOperand<32>";
  let ParserMatchClass = UImm32SymAsmOperand;
}

// 32-bit absolute branch/call target address (word 1, FK_KlaussCPU_ABS32).
def BrTarget32 : Operand<OtherVT> {
  let EncoderMethod    = "getAbs32OpValue";
  let DecoderMethod    = "decodeUImmOperand<32>";
  let ParserMatchClass = UImm32SymAsmOperand;
}

def CallTarget32 : Operand<i64> {
  let EncoderMethod    = "getAbs32OpValue";
  let DecoderMethod    = "decodeUImmOperand<32>";
  let ParserMatchClass = UImm32SymAsmOperand;
}

// PC-relative branch/call target and LEAPC offset: a 32-bit byte
// displacement from the instruction in word 1 (FK_KlaussCPU_PCREL32).
def PCRelTarget32 : Operand<OtherVT> {
  let EncoderMethod    = "getPCRel32OpValue";
  let DecoderMethod    = "decodeSImmOperand<32>";
  let ParserMatchClass = PCRel32AsmOperand;
}

def PCRelCallTarget32 : Operand<i64> {
  let EncoderMethod    = "getPCRel32OpValue";
  let DecoderMethod    = "decodeSImmOperand<32>";
  let ParserMatchClass = PCRel32AsmOperand;
}

def pcrel_simm32 : Operand<i64> {
  let EncoderMethod    = "getPCRel32OpValue";
  let DecoderMethod    = "decodeSImmOperand<32>";
  let ParserMatchClass = PCRel32AsmOperand;
}

// ISA v3 short branch target: byte displacement, encoded as a simm18 word
// displacement in word0[17:0] (FK_KlaussCPU_PCREL18).
def PCRelTarget18 : Operand<OtherVT> {
  let EncoderMethod    = "getPCRel18OpValue";
  let DecoderMethod    = "decodeSImmOperand<20>";
  let ParserMatchClass = PCRel20AsmOperand;
}

// ISA v3 fused-branch target: byte displacement, encoded as a simm13 word
// displacement in word0[20:8] (FK_KlaussCPU_PCREL13).
def PCRelTarget13 : Operand<OtherVT> {
  let EncoderMethod    = "getPCRel13OpValue";
  let DecoderMethod    = "decodeSImmOperand<15>";
  let ParserMatchClass = PCRel15AsmOperand;
}

// Fused-branch condition (PRED | INV << 3) and register-immediate operand.
// The CPU traps on PRED 5..7, so those neither assemble nor decode.
def fbr_cc : Operand<i64> {
  let DecoderMethod    = "decodeFBRCondOperand";
  let ParserMatchClass = FBRCondAsmOperand;
}

def fbr_simm4 : Operand<i64> {
  let DecoderMethod    = "decodeSImmOperand<4>";
  let ParserMatchClass = SImm4AsmOperand;
}

//===----------------------------------------------------------------------===//
// Instruction base classes
//===----------------------------------------------------------------------===//

class KCInst<int size, bits<32> word0, dag outs, dag ins, string asmstr,
             list<dag> pat>
    : Instruction {
  let Namespace        = "KlaussCPU";
  let Size             = size;
  dag OutOperandList   = outs;
  dag InOperandList    = ins;
  let AsmString        = asmstr;
  let Pattern          = pat;
  let DecoderNamespace = "KlaussCPU";
  let SchedRW          = [WriteALU];   // default; overridden per format (M8)
  // Word-0 template (operand fields zero); the short forms derive theirs
  // from their 2-word twin's.
  bits<32> Word0 = word0;
  // Retired v1 peripheral opcodes keep their v1 word (LEN=00, which the v2
  // CPU traps on); every other template's LEN must match Size.  They do not
  // disassemble.
  bit isRetiredV1 = 0;
  let isAsmParserOnly = isRetiredV1;
}

class KCInst32<bits<32> word0, dag outs, dag ins, string asmstr,
               list<dag> pat = []>
    : KCInst<4, word0, outs, ins, asmstr, pat> {
  field bits<32> Inst = word0;
  field bits<32> SoftFail = 0;
  assert !or(isRetiredV1, !eq(word0{31-30}, 0b01)),
         NAME # ": a 1-word template needs LEN=01";
}

// imm32 / target in word 1.
class KCInst64<bits<32> word0, dag outs, dag ins, string asmstr,
               list<dag> pat = []>
    : KCInst<8, word0, outs, ins, asmstr, pat> {
  field bits<64> Inst;
  field bits<64> SoftFail = 0;
  let Inst{31-0} = word0;
  assert !or(isRetiredV1, !eq(word0{31-30}, 0b10)),
         NAME # ": a 2-word template needs LEN=10";
}

// imm64: lo32 in word 1, hi32 in word 2.
class KCInst96<bits<32> word0, dag outs, dag ins, string asmstr,
               list<dag> pat = []>
    : KCInst<12, word0, outs, ins, asmstr, pat> {
  field bits<96> Inst;
  field bits<96> SoftFail = 0;
  let Inst{31-0} = word0;
  assert !eq(word0{31-30}, 0b11), NAME # ": a 3-word template needs LEN=11";
}

//===----------------------------------------------------------------------===//
// Three registers: rd = rs1 op rs2
//===----------------------------------------------------------------------===//

class RRR<bits<32> word0, dag outs, dag ins, string asmstr,
          list<dag> pat = []>
    : KCInst32<word0, outs, ins, asmstr, pat> {
  bits<4> rd;
  bits<4> rs1;
  bits<4> rs2;
  let Inst{11-8} = rd;
  let Inst{7-4}  = rs1;
  let Inst{3-0}  = rs2;
  let Defs       = [FLAGS];   // arith/cmp/shift/mul/div set flags (bitwise over-marked; safe)
}

//===----------------------------------------------------------------------===//
// Two registers
//===----------------------------------------------------------------------===//

// 1 output, 1 input: rd, rs1 (COPY, MEMGET*)
class RR1<bits<32> word0, dag outs, dag ins, string asmstr,
          list<dag> pat = []>
    : KCInst32<word0, outs, ins, asmstr, pat> {
  bits<4> rd;
  bits<4> rs;
  let Inst{11-8} = rd;
  let Inst{7-4}  = rs;
  let SchedRW    = [WriteLoad];   // MEMGET*/MEMREADRR (COPY_R overrides to WriteALU)
}

// 0 outputs, 2 source inputs: rs1, rs2 (CMPRR)
class RR0<bits<32> word0, string asmstr, list<dag> pat = []>
    : KCInst32<word0, (outs), (ins GPR:$rs1, GPR:$rs2), asmstr, pat> {
  bits<4> rs1;
  bits<4> rs2;
  let Inst{7-4} = rs1;
  let Inst{3-0} = rs2;
  let Defs      = [FLAGS];    // CMPRR sets flags
}

// Store: data in rd, address in rs1, no output (MEMSET*)
class RRst<bits<32> word0, string asmstr, list<dag> pat = []>
    : KCInst32<word0, (outs), (ins GPR:$rs_data, GPR:$rs_addr), asmstr, pat> {
  bits<4> rs_data;
  bits<4> rs_addr;
  let Inst{11-8} = rs_data;
  let Inst{7-4}  = rs_addr;
  let mayStore   = 1;
  let SchedRW    = [WriteStore];
}

// Register-offset indexed 64-bit load/store: EA = rs1 + rs2 (the offset
// register is a real GPR operand in rs2).
class Idx64RegOff_ld<bits<32> word0, string asmstr>
    : KCInst32<word0, (outs GPR:$rd), (ins GPR:$base, GPR:$offset), asmstr> {
  bits<4> rd;
  bits<4> base;
  bits<4> offset;
  let Inst{11-8} = rd;
  let Inst{7-4}  = base;
  let Inst{3-0}  = offset;
  let mayLoad    = 1;
  let SchedRW    = [WriteLoad];
}
class Idx64RegOff_st<bits<32> word0, string asmstr>
    : KCInst32<word0, (outs), (ins GPR:$rs_data, GPR:$base, GPR:$offset),
               asmstr> {
  bits<4> rs_data;
  bits<4> base;
  bits<4> offset;
  let Inst{11-8} = rs_data;
  let Inst{7-4}  = base;
  let Inst{3-0}  = offset;
  let mayStore   = 1;
  let SchedRW    = [WriteStore];
}

//===----------------------------------------------------------------------===//
// Single register
//===----------------------------------------------------------------------===//

// In-place with 2-address constraint: rd = f(rd); only rd is encoded.
class R_inplace<bits<32> word0, string asmstr, list<dag> pat = []>
    : KCInst32<word0, (outs GPR:$rd), (ins GPR:$rs1), asmstr, pat> {
  bits<4> rd;
  let Inst{11-8}  = rd;
  let Constraints = "$rd = $rs1";
  let Defs        = [FLAGS];   // inc/dec/neg/shift-by-1/sext set flags (over-marked; safe)
}

// Unary 3-operand form: rd = f(rs1).  ISA v2 encodes rd and rs1 separately
// ("rd≠rs1 also works, same hardware path" — ISA_ENCODING_V2.md §4), so
// these are NOT tied; the register allocator only picks rd==rs1 when the
// source is dead.  The old 1-operand spelling (`incr r1`) stays accepted via
// InstAlias (see the end of this file).
class R_unary<bits<32> word0, string mnem, list<dag> pat = []>
    : KCInst32<word0, (outs GPR:$rd), (ins GPR:$rs1), mnem # "\t$rd, $rs1",
               pat> {
  bits<4> rd;
  bits<4> rs1;
  let Inst{11-8} = rd;
  let Inst{7-4}  = rs1;
  let Defs       = [FLAGS];   // inc/dec/neg/sext/... set flags (over-marked; safe)
}

// Input-only, source in rs1: PUSH, SETSP, DELAYR, LCDDATAR
class R_in<bits<32> word0, dag ins, string asmstr, list<dag> pat = []>
    : KCInst32<word0, (outs), ins, asmstr, pat> {
  bits<4> reg;
  let Inst{7-4} = reg;
}

// Input-only, register target in rs2: JMPR, CALLR
class R_ind<bits<32> word0, dag ins, string asmstr>
    : KCInst32<word0, (outs), ins, asmstr> {
  bits<4> reg;
  let Inst{3-0} = reg;
}

// Output-only, rd: POP, GETSP
class R_out<bits<32> word0, dag outs, string asmstr, list<dag> pat = []>
    : KCInst32<word0, outs, (ins), asmstr, pat> {
  bits<4> reg;
  let Inst{11-8} = reg;
}

// Retired v1 peripheral op (LED / 7-seg / RGB): the v1 word with the
// register in [3:0].  LEN=00, so the v2 CPU traps (ERR_INV_OPCODE) if one
// executes, which is the intended fail-fast behaviour; they exist only so
// old hand-written inline asm still assembles.  Codegen never emits them.
class RetiredV1_R<bits<32> word0, string asmstr>
    : KCInst32<word0, (outs), (ins GPR:$reg), asmstr> {
  bits<4> reg;
  let Inst{3-0}   = reg;
  let isRetiredV1 = 1;
}

//===----------------------------------------------------------------------===//
// No operands: the template is the whole instruction
//===----------------------------------------------------------------------===//

class I0<bits<32> word0, dag outs, dag ins, string asmstr, list<dag> pat = []>
    : KCInst32<word0, outs, ins, asmstr, pat>;

//===----------------------------------------------------------------------===//
// Class 4 shift/rotate/bit by an embedded constant N in word0[20:15]
//===----------------------------------------------------------------------===//

// 3-operand (rd and rs independent in v2).  The count/position operand is a
// 6-bit constant (uimm6).
class ShiftImmN<bits<32> word0, string mnem, list<dag> pat = []>
    : KCInst32<word0, (outs GPR:$rd), (ins GPR:$rs, uimm6:$imm),
               mnem # "\t$rd, $rs, $imm", pat> {
  bits<4> rd;
  bits<4> rs;
  bits<6> imm;
  let Inst{20-15} = imm;
  let Inst{11-8}  = rd;
  let Inst{7-4}   = rs;
  let Defs        = [FLAGS];   // shlv/shrv/rolv/rorv set flags
}

// Bit test by embedded constant N — flag-only, no destination register
// (Z = ~bit).
class BitTestImmN<bits<32> word0, string asmstr>
    : KCInst32<word0, (outs), (ins GPR:$rs, uimm6:$imm), asmstr> {
  bits<4> rs;
  bits<6> imm;
  let Inst{20-15} = imm;
  let Inst{7-4}   = rs;
  let Defs        = [FLAGS];   // Z = ~bit
}

//===----------------------------------------------------------------------===//
// Register + 32-bit immediate in word 1
//===----------------------------------------------------------------------===//

// Load immediate / address into register: rd = imm32
class RV_ld<bits<32> word0, string asmstr, list<dag> pat = [],
            Operand immop = simm32>
    : KCInst64<word0, (outs GPR:$rd), (ins immop:$imm), asmstr, pat> {
  bits<4>  rd;
  bits<32> imm;
  let Inst{63-32} = imm;
  let Inst{11-8}  = rd;
}

// Store a register to an absolute address: data in rd, address in word 1.
// No Defs: stores leave FLAGS alone (class-7 decode sets no fmask).
class RV_st<bits<32> word0, string asmstr>
    : KCInst64<word0, (outs), (ins GPR:$rs_data, uimm32_abs:$addr), asmstr> {
  bits<4>  rs_data;
  bits<32> addr;
  let Inst{63-32} = addr;
  let Inst{11-8}  = rs_data;
}

// 3-operand immediate form: rd = rs op imm32 (untied — see R_unary).
class RV_imm<bits<32> word0, string mnem, Operand immop = simm32>
    : KCInst64<word0, (outs GPR:$rd), (ins GPR:$rs, immop:$imm),
               mnem # "\t$rd, $rs, $imm"> {
  bits<4>  rd;
  bits<4>  rs;
  bits<32> imm;
  let Inst{63-32} = imm;
  let Inst{11-8}  = rd;
  let Inst{7-4}   = rs;
  let Defs        = [FLAGS];   // addv/minusv set flags (bitwise-imm over-marked; safe)
}

// Three registers + imm32: rd = f(rs1, rs2, imm32) (BDEP).
class RRRV<bits<32> word0, string mnem, Operand immop>
    : KCInst64<word0, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2, immop:$imm),
               mnem # "\t$rd, $rs1, $rs2, $imm"> {
  bits<4>  rd;
  bits<4>  rs1;
  bits<4>  rs2;
  bits<32> imm;
  let Inst{63-32} = imm;
  let Inst{11-8}  = rd;
  let Inst{7-4}   = rs1;
  let Inst{3-0}   = rs2;
}

// Compare register to immediate — flags only, no output (CMPRV)
class RV_cmp<bits<32> word0, string asmstr>
    : KCInst64<word0, (outs), (ins GPR:$rs, simm32:$imm), asmstr> {
  bits<4>  rs;
  bits<32> imm;
  let Inst{63-32} = imm;
  let Inst{7-4}   = rs;
  let Defs        = [FLAGS];   // CMPRV sets flags
}

//===----------------------------------------------------------------------===//
// Two registers + 32-bit immediate / offset in word 1
//===----------------------------------------------------------------------===//

// ALU variant: rd = rs op sign_extend(imm32) — no memory side-effect.
class RRV_alu<bits<32> word0, string asmstr, list<dag> pat = []>
    : KCInst64<word0, (outs GPR:$rd), (ins GPR:$rs, simm32:$imm), asmstr,
               pat> {
  bits<4>  rd;
  bits<4>  rs;
  bits<32> imm;
  let Inst{63-32} = imm;
  let Inst{11-8}  = rd;
  let Inst{7-4}   = rs;
  let Defs        = [FLAGS];   // addi sets flags
}

// Indexed load: rd = mem[base + offset]
class RRV_ld<bits<32> word0, string asmstr, list<dag> pat = []>
    : KCInst64<word0, (outs GPR:$rd), (ins GPR:$base, simm32:$offset),
               asmstr, pat> {
  bits<4>  rd;
  bits<4>  base;
  bits<32> offset;
  let Inst{63-32} = offset;
  let Inst{11-8}  = rd;
  let Inst{7-4}   = base;
  let mayLoad     = 1;
  let SchedRW     = [WriteLoad];
}

// Indexed store: mem[base + offset] = rs_data
class RRV_st<bits<32> word0, string asmstr, list<dag> pat = []>
    : KCInst64<word0, (outs), (ins GPR:$rs_data, GPR:$base, simm32:$offset),
               asmstr, pat> {
  bits<4>  rs_data;
  bits<4>  base;
  bits<32> offset;
  let Inst{63-32} = offset;
  let Inst{11-8}  = rs_data;
  let Inst{7-4}   = base;
  let mayStore    = 1;
  let SchedRW     = [WriteStore];
}

//===----------------------------------------------------------------------===//
// Fixed word 0 + 32-bit target or immediate in word 1
//===----------------------------------------------------------------------===//

// Unconditional branch
class Vbr_uncond<bits<32> word0, string asmstr, list<dag> pat = [],
                 Operand tgt = BrTarget32>
    : KCInst64<word0, (outs), (ins tgt:$target), asmstr, pat> {
  bits<32> target;
  let Inst{63-32}  = target;
  let isBranch     = 1;
  let isTerminator = 1;
  let isBarrier    = 1;
  let SchedRW      = [WriteBranch];
}

// Conditional branch (flags set by preceding CMPRR/CMPRV)
class Vbr_cond<bits<32> word0, string asmstr, Operand tgt = BrTarget32>
    : KCInst64<word0, (outs), (ins tgt:$target), asmstr> {
  bits<32> target;
  let Inst{63-32}  = target;
  let isBranch     = 1;
  let isTerminator = 1;
  let SchedRW      = [WriteBranch];
  let Uses         = [FLAGS];   // conditional branch reads the flags a compare set
}

// PC-relative conditional branch
class Vbr_relcond<bits<32> word0, string asmstr>
    : Vbr_cond<word0, asmstr, PCRelTarget32>;

// Direct call
class Vcall<bits<32> word0, string asmstr, Operand tgt = CallTarget32>
    : KCInst64<word0, (outs), (ins tgt:$target), asmstr> {
  bits<32> target;
  let Inst{63-32} = target;
  let isCall      = 1;
  let SchedRW     = [WriteBranch];
}

// SP adjustment (ADDSP), push and peripheral immediates
class Vsp<bits<32> word0, string asmstr, Operand immop = simm32>
    : KCInst64<word0, (outs), (ins immop:$imm), asmstr> {
  bits<32> imm;
  let Inst{63-32} = imm;
  let SchedRW     = [WriteSP];
}

//===----------------------------------------------------------------------===//
// 64-bit immediate: lo32 in word 1, hi32 in word 2 (3 words)
//===----------------------------------------------------------------------===//

// rd = (hi32 << 32) | lo32
class V64_ld<bits<32> word0, string asmstr, list<dag> pat = []>
    : KCInst96<word0, (outs GPR:$rd), (ins imm32:$lo, imm32:$hi), asmstr,
               pat> {
  bits<4>  rd;
  bits<32> lo;
  bits<32> hi;
  let Inst{95-64} = hi;
  let Inst{63-32} = lo;
  let Inst{11-8}  = rd;
}

// Push (hi32 << 32) | lo32
class V64_imm<bits<32> word0, string asmstr>
    : KCInst96<word0, (outs), (ins imm32:$lo, imm32:$hi), asmstr> {
  bits<32> lo;
  bits<32> hi;
  let Inst{95-64} = hi;
  let Inst{63-32} = lo;
  let SchedRW     = [WriteSP];
}

//===----------------------------------------------------------------------===//
// ALU — 3-register (RRR)
//===----------------------------------------------------------------------===//

// Arithmetic
def ADDR   : RRR<0x44200000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "addr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (add GPR:$rs1, GPR:$rs2))]>;
def SUBR   : RRR<0x44600000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "subr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (sub GPR:$rs1, GPR:$rs2))]>;

// Bitwise
def ANDR   : RRR<0x45000000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "andr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (and GPR:$rs1, GPR:$rs2))]>;
def ORR    : RRR<0x45400000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "orr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (or  GPR:$rs1, GPR:$rs2))]>;
def XORR   : RRR<0x45800000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "xorr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (xor GPR:$rs1, GPR:$rs2))]>;

// ISA v3 D3: 32-bit "W" ops — the 64-bit op with its result sign-extended
// from bit 31 (RV64-style: C `int` arithmetic stays sign-extended without a
// following SEXTW). Class 1 / class 2 (2-word) / class A with W [19]. Selected
// in KlaussCPUDAGToDAGISel::Select (-mllvm -klausscpu-w32-alu=false).
// (No isel Pattern, so side effects are stated explicitly — see SETR64.)
let hasSideEffects = 0 in {
def ADDW   : RRR<0x44280000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "addw\t$rd, $rs1, $rs2">;
def SUBW   : RRR<0x44680000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "subw\t$rd, $rs1, $rs2">;
def ADDIW  : RRV_alu<0x88380000, "addiw\t$rd, $rs, $imm">;
let SchedRW = [WriteMul] in
def MULW   : RRR<0x68880000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "mulw\t$rd, $rs1, $rs2">;
}

// Multiply (EX-serialized, ~5c — M8)
let SchedRW = [WriteMul] in {
def MULR   : RRR<0x68800000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "mulr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (mul GPR:$rs1, GPR:$rs2))]>;
def MULUR  : RRR<0x68000000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "mulur\t$rd, $rs1, $rs2">;          // unsigned lo == signed lo; no extra pattern
def MULHR  : RRR<0x68C00000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "mulhr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (mulhs GPR:$rs1, GPR:$rs2))]>;
def MULHUR : RRR<0x68400000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "mulhur\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (mulhu GPR:$rs1, GPR:$rs2))]>;
}

// Divide / modulo (EX-serialized, iterative — M8)
let SchedRW = [WriteDiv] in {
def DIVR   : RRR<0x69800000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "divr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (sdiv GPR:$rs1, GPR:$rs2))]>;
def DIVUR  : RRR<0x69000000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "divur\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (udiv GPR:$rs1, GPR:$rs2))]>;
def MODR   : RRR<0x6A800000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "modr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (srem GPR:$rs1, GPR:$rs2))]>;
def MODUR  : RRR<0x6A000000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "modur\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (urem GPR:$rs1, GPR:$rs2))]>;
}

// Shifts
def SHLR   : RRR<0x50004000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "shlr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (shl GPR:$rs1, GPR:$rs2))]>;
def SHRR   : RRR<0x50404000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "shrr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (srl GPR:$rs1, GPR:$rs2))]>;
def SARR   : RRR<0x50804000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "sarr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (sra GPR:$rs1, GPR:$rs2))]>;

// Rotate — selected from ISD::ROTL/ROTR (i64 Legal in ISelLowering).
// Rust's u64::rotate_left/right and hash/crypto kernels hit these hard.
def ROLR   : RRR<0x50C04000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "rolr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (rotl GPR:$rs1, GPR:$rs2))]>;
def RORR_R : RRR<0x51004000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "rorr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (rotr GPR:$rs1, GPR:$rs2))]>;

// Compare-and-materialise: rd = (rs1 cond rs2) ? 1 : 0
def CMPEQR  : RRR<0x4C200000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpeqr\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETEQ)))]>;
def CMPNER  : RRR<0x4C600000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpner\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETNE)))]>;
def CMPLTR  : RRR<0x4CA00000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpltr\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETLT)))]>;
def CMPLER  : RRR<0x4D200000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpler\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETLE)))]>;
def CMPGTR  : RRR<0x4D600000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpgtr\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETGT)))]>;
def CMPGER  : RRR<0x4CE00000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpger\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETGE)))]>;
def CMPULTR : RRR<0x4DA00000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpultr\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETULT)))]>;
def CMPULER : RRR<0x4E200000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpuler\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETULE)))]>;
def CMPUGTR : RRR<0x4E600000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpugtr\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETUGT)))]>;
def CMPUGER : RRR<0x4DE00000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpuger\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETUGE)))]>;

// Min / Max
def MINR   : RRR<0x45C00000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "minr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (smin GPR:$rs1, GPR:$rs2))]>;
def MAXR   : RRR<0x46000000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "maxr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (smax GPR:$rs1, GPR:$rs2))]>;
def MINUR  : RRR<0x46400000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "minur\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (umin GPR:$rs1, GPR:$rs2))]>;
def MAXUR  : RRR<0x46800000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "maxur\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (umax GPR:$rs1, GPR:$rs2))]>;

//===----------------------------------------------------------------------===//
// ALU — 2-register (RR)
//===----------------------------------------------------------------------===//

// Register copy: rd = rs
// Pattern-less, so TableGen would otherwise guess hasSideEffects=1 and every
// post-RA copy would become a scheduling barrier.
let hasSideEffects = 0, isMoveReg = 1, isAsCheapAsAMove = 1 in
def COPY_R   : RR1<0x54000000, (outs GPR:$rd), (ins GPR:$rs),
                  "copy\t$rd, $rs"> { let SchedRW = [WriteALU]; } // move, not a load

// Compare (flags only): rs1 - rs2, no writeback.
// No hasSideEffects: the compare→branch pairing is held by glue at isel and by
// the modelled FLAGS def/use afterwards (every flag-writing instruction
// declares Defs=[FLAGS], conditional branches Uses=[FLAGS]).  A side-effect
// flag would only make it a scheduling/LICM barrier for unrelated loads/stores.
let hasSideEffects = 0 in
def CMPRR_I  : RR0<0x4C000000, "cmprr\t$rs1, $rs2">;
// ISA v3 D2: 32-bit compare of the low halves (class 3 W bit [19]). Flags
// are exactly those of the 32-bit subtraction; selected by BR_CC when both
// operands are 32-bit extended (see KlaussCPUDAGToDAGISel::Select).
let hasSideEffects = 0 in {
def CMPRRW_I : RR0<0x4C080000, "cmprrw\t$rs1, $rs2">;
def CMPRVW_I : RV_cmp<0x8C180000, "cmprvw\t$rs, $imm">;
}

// 64-bit load / store via absolute register address
def MEMGET64 : RR1<0x5B100000, (outs GPR:$rd), (ins GPR:$rs),
                  "memget64\t$rd, $rs"> {
  let mayLoad = 1;
}
def MEMSET64 : RRst<0x5F100000, "memset64\t$rs_data, $rs_addr">;

// Sub-word register-addressed loads (zero-extend to 64 bits, pure GPR address).
// Indexed variants (base+offset) are LDIDX8/16/32 below.
def MEMGET8  : RR1<0x58000000, (outs GPR:$rd), (ins GPR:$rs),
                  "memget8\t$rd, $rs"> { let mayLoad = 1; }
def MEMGET16 : RR1<0x59000000, (outs GPR:$rd), (ins GPR:$rs),
                  "memget16\t$rd, $rs"> { let mayLoad = 1; }
def MEMGET32 : RR1<0x5A000000, (outs GPR:$rd), (ins GPR:$rs),
                  "memget32\t$rd, $rs"> { let mayLoad = 1; }

// Sub-word register-addressed stores (stores low N bits of rs_data to mem[rs_addr]).
//
// ALIGNMENT WARNING: MEMSET16/MEMSET32 silently mask the low address bits in
// hardware (mem16[rs2 & ~1], mem32[rs2 & ~3] — CPU_ARCHITECTURE.md §8.1). An
// under-aligned store writes the *aligned* slot with NO fault → silent memory
// corruption, not a trap. These (and STIDX16/STIDX32 below) are only safe
// because allowsMisalignedMemoryAccesses() is left at its default (false), so
// the legalizer expands every under-aligned i16/i32 store to byte ops (MEMSET8/
// STIDX8) and never selects a masking store on an unaligned address.
// If misaligned access is ever enabled (e.g. to get cheap unaligned i32 *loads*
// for crypto), scope that permission to the LOAD path only — do NOT let the
// store path reach these opcodes on an unaligned address; route unaligned
// stores through STIDX8/MEMSET8, or switch to a HW store that faults instead of
// masking. (Same invariant as the force-aligned LDIDX64/STIDX64 note above.)
def MEMSET8  : RRst<0x5C000000, "memset8\t$rs_data, $rs_addr">;
def MEMSET16 : RRst<0x5D000000, "memset16\t$rs_data, $rs_addr">;
def MEMSET32 : RRst<0x5E000000, "memset32\t$rs_data, $rs_addr">;

//===----------------------------------------------------------------------===//
// ALU — single-register in-place (2-address: $rd = $rs1)
//===----------------------------------------------------------------------===//

def INCR    : R_unary<0x57880000, "incr">;
def DECR    : R_unary<0x57C80000, "decr">;
def NEGR    : R_unary<0x54480000, "negr",
                [(set GPR:$rd, (ineg GPR:$rs1))]>;
def ABSR    : R_unary<0x54C80000, "absr",
                [(set GPR:$rd, (abs GPR:$rs1))]>;
def SHLR1   : R_unary<0x50208000, "shlr1">;
def SHRR1   : R_unary<0x50608000, "shrr1">;
def NOTR    : R_unary<0x54880000, "notr",
                [(set GPR:$rd, (not GPR:$rs1))]>;
def POPCNT  : R_unary<0x56080000, "popcnt",
                [(set GPR:$rd, (ctpop GPR:$rs1))]>;
def CLZ     : R_unary<0x56400000, "clz",
                [(set GPR:$rd, (ctlz GPR:$rs1))]>;
def CTZ     : R_unary<0x56800000, "ctz",
                [(set GPR:$rd, (cttz GPR:$rs1))]>;
def BITREV  : R_unary<0x55C00000, "bitrev",
                [(set GPR:$rd, (bitreverse GPR:$rs1))]>;
def BSWAP_R : R_unary<0x55800000, "bswap",
                [(set GPR:$rd, (bswap GPR:$rs1))]>;

// Sign-extend to 64 bits (arch doc §7.2; hardware extends to 64)
def SEXTB   : R_unary<0x55080000, "sextb",
                [(set GPR:$rd, (sext_inreg GPR:$rs1, i8))]>;
def SEXTH   : R_unary<0x55180000, "sexth",
                [(set GPR:$rd, (sext_inreg GPR:$rs1, i16))]>;

// Zero-extend to 64 bits
def ZEXTB   : R_unary<0x55480000, "zextb">;
def ZEXTH   : R_unary<0x55580000, "zexth">;

// Sign/zero-extend 32→64 bits (hardware fixed April 2026)
def SEXTW   : R_unary<0x55200000, "sextw",
                [(set GPR:$rd, (sext_inreg GPR:$rs1, i32))]>;
def ZEXTW   : R_unary<0x55600000, "zextw">;

//===----------------------------------------------------------------------===//
// Pseudo-instructions (not emitted to the binary; eliminated by PEI)
//===----------------------------------------------------------------------===//

class Pseudo<dag outs, dag ins, list<dag> pattern = []>
    : Instruction {
  let Namespace        = "KlaussCPU";
  dag OutOperandList   = outs;
  dag InOperandList    = ins;
  let AsmString        = "";
  let Pattern          = pattern;
  let isPseudo         = 1;
  let isCodeGenOnly    = 1;
  let Size             = 0;
  // Eliminated before scheduling (PEI / custom inserter), so they never reach
  // the scheduler — required for CompleteModel=1 (KlaussCPUSchedule.td).
  let hasNoSchedulingInfo = 1;
  field bits<32> Inst  = 0;
  field bits<32> SoftFail = 0;
  let DecoderNamespace = "KlaussCPU";
}

// Call-frame setup/destroy — consumed by eliminateCallFramePseudoInstr.
// No tablegen patterns: ISD::CALLSEQ_START/END are matched in
// KlaussCPUDAGToDAGISel::Select() to avoid i32-type-set inference issues
// (KlaussCPU has no i32 register class).
let Defs = [SP], Uses = [SP], hasSideEffects = 1 in {
def ADJCALLSTACKDOWN : Pseudo<(outs), (ins i32imm:$amt1, i32imm:$amt2)>;
def ADJCALLSTACKUP   : Pseudo<(outs), (ins i32imm:$amt1, i32imm:$amt2)>;
}

// SELECT without CMOV: KlaussCPUISD::SELECT_CC (lhs, rhs, cc, t, f) selects
// to one of these; EmitInstrWithCustomInserter (KlaussCPUISelLowering.cpp)
// expands it to CMPRR/CMPRV + the conditional jump in $jmp + a PHI.
// $jmp holds the JMPxx opcode (from KlaussCPUInstrInfo::getCondBranchOpcode).
// Defs=[FLAGS]: the expansion's compare clobbers the flags.
let usesCustomInserter = 1, hasSideEffects = 0, Defs = [FLAGS] in {
def SELECT_CC_RR : Pseudo<(outs GPR:$dst),
                          (ins GPR:$lhs, GPR:$rhs, i64imm:$jmp,
                               GPR:$trueV, GPR:$falseV)>;
def SELECT_CC_RI : Pseudo<(outs GPR:$dst),
                          (ins GPR:$lhs, simm32:$rhs, i64imm:$jmp,
                               GPR:$trueV, GPR:$falseV)>;
}

//===----------------------------------------------------------------------===//
// Stack management (used by frame lowering; no isel patterns)
//===----------------------------------------------------------------------===//

let Uses = [SP], Defs = [SP], mayStore = 1 in
def PUSH_R  : R_in<0x64000000, (ins GPR:$reg), "push\t$reg">;
let Uses = [SP], Defs = [SP], mayLoad = 1 in
def POP_R   : R_out<0x64800000, (outs GPR:$reg), "pop\t$reg">;
let Uses = [SP], hasSideEffects = 1 in
def GETSP_R : R_out<0x64C00000, (outs GPR:$reg), "getsp\t$reg">;
let Defs = [SP], hasSideEffects = 1 in
def SETSP_R : R_in<0x65000000, (ins GPR:$reg), "setsp\t$reg">;
let Uses = [SP], Defs = [SP], hasSideEffects = 1 in
def ADDSP_I : Vsp<0xA5400000, "addsp\t$imm">;

// ISA v3 C (frame setup/teardown, one word each; no isel patterns — built by
// KlaussCPUFrameLowering):
//   ENTER N : push R15; R15 = SP; SP -= 8*N      (N = frame dwords, [21:0])
//   LEAVE   : SP = R15; pop R15
let Uses = [SP, R15], Defs = [SP, R15], hasSideEffects = 1, mayStore = 1 in
def ENTER_I : KCInst32<0x66000000, (outs), (ins uimm22:$n), "enter\t$n"> {
  bits<22> n;
  let Inst{21-0} = n;
}
let Uses = [R15], Defs = [SP, R15], hasSideEffects = 1, mayLoad = 1 in
def LEAVE_I : I0<0x66400000, (outs), (ins), "leave">;
//   LEAVERET: LEAVE; RET  (R15 = [R15], PC = [R15+8], SP = R15 + 16)
let isReturn = 1, isTerminator = 1, isBarrier = 1,
    Uses = [R15], Defs = [SP, R15], hasSideEffects = 1, mayLoad = 1 in
def LEAVERET_I : I0<0x66800000, (outs), (ins), "leaveret">;

// Indirect call / jump via register
// Defs: all caller-saved GPRs clobbered by the ABI (R0-R3=args, R8-R14=temps+return)
// plus FLAGS (the callee is free to change them).
let isCall = 1,
    Uses = [SP],
    Defs = [R0, R1, R2, R3, R8, R9, R10, R11, R12, R13, R14, FLAGS] in
def CALLR_R : R_ind<0x62800000, (ins GPR:$reg), "callr\t$reg">;
def JMPR_R  : R_ind<0x60800000, (ins GPR:$reg), "jmpr\t$reg"> {
  let isBranch         = 1;
  let isIndirectBranch = 1;
  let isTerminator     = 1;
  let isBarrier        = 1;  // indirect branch never falls through
}

//===----------------------------------------------------------------------===//
// RV: register ← immediate  (SETR and in-place RV ops)
//===----------------------------------------------------------------------===//

// Constant materialisation.  Explicitly side-effect free and rematerializable:
// SETR64/LEAPC have no isel Pattern, so TableGen would otherwise guess
// hasSideEffects=1, which blocks MachineLICM hoisting, MachineCSE and remat
// (a 64-bit constant in a loop would be re-emitted, or spilled, every trip).
let hasSideEffects = 0, isReMaterializable = 1 in {
// Load 32-bit immediate (sign-extended to 64 bits)
let isAsCheapAsAMove = 1, isMoveImm = 1 in
def SETR    : RV_ld<0x8BD00000, "setr\t$rd, $imm", [], simm32_abs>;

// Load full 64-bit immediate: rd = (hi32 << 32) | lo32
let isMoveImm = 1 in
def SETR64  : V64_ld<0xCBC00000, "setr64\t$rd, $lo, $hi">;
}

// In-place immediate operations (rd op= imm32); the CPU zero-extends the
// imm32 of ADDV/MINUSV/ANDV/ORV/XORV and sign-extends the MULV/DIVV/MODV one.
def ADDV    : RV_imm<0x88200000, "addv", uimm32>;
def MINUSV  : RV_imm<0x88600000, "minusv", uimm32>;
def ANDV    : RV_imm<0x89000000, "andv", uimm32>;
def ORV     : RV_imm<0x89400000, "orv", uimm32>;
def XORV    : RV_imm<0x89800000, "xorv", uimm32>;
// Shift by immediate (count = imm[5:0]); v2: single word, count embedded.
def SHLV    : ShiftImmN<0x50204000, "shlv">;
def SHRV    : ShiftImmN<0x50604000, "shrv">;
def SHRAV   : ShiftImmN<0x50A04000, "shrav">;
def MULV    : RV_imm<0xA8800000, "mulv">;
def DIVV    : RV_imm<0xA9800000, "divv">;
def MODV    : RV_imm<0xAA800000, "modv">;

// Rotate by immediate (count = imm[5:0]); v2: single word.  Register-amount
// forms are ROLR/RORR_R.
def ROLV    : ShiftImmN<0x50E04000, "rolv">;
def RORV    : ShiftImmN<0x51204000, "rorv">;

// reg+imm32 add: rd = rs + sign_extend(imm32). Two distinct regs; sign-extends.
// Replaces the SETR+ADDR two-instruction sequence for frame-address materialisation.
// Pattern deferred: simm32_imm is not yet defined here; see isel patterns section.
def ADDI : RRV_alu<0x88300000, "addi\t$rd, $rs, $imm">;

// Compare register to immediate — flags only (see CMPRR_I on side effects)
let hasSideEffects = 0 in
def CMPRV_I : RV_cmp<0x8C100000, "cmprv\t$rs, $imm">;

//===----------------------------------------------------------------------===//
// Memory: indexed load / store  (primary frame-slot instructions)
//
// Class 6 (load) / 7 (store), MODE=01 (rs1 + imm32): word 0 holds
// SIZE [25:24], SGN [23] and A [20]; rd = destination / store data,
// rs1 = base, word 1 = offset32.  Sub-word loads zero-extend unless SGN=1.
//
// IMPORTANT: ldidx64/stidx64 (0x9B300000/0x9F300000) are the *force-aligned*
// hardware variants (A=1, vh LDIDX64A/STIDX64A) — the RTL computes
// (rs1 + imm) & ~7 before the bus access.  The non-aligned forms are
// ldidx64u/stidx64u (A=0).  Using the force-aligned form is only safe because
// LLVM never emits a misaligned i64 access: allowsMisalignedMemoryAccesses()
// is left at its default (false), so under-aligned i64 loads/stores are
// expanded to byte ops by the legalizer.  If misaligned i64 access is ever
// enabled, switch these to the A=0 forms first (and confirm the HW handles a
// 64-bit read spanning two cache lines).
//===----------------------------------------------------------------------===//

def LDIDX64 : RRV_ld<0x9B300000, "ldidx64\t$rd, $base, $offset">;
def STIDX64 : RRV_st<0x9F300000, "stidx64\t$rs_data, $base, $offset">;

def LDIDX32 : RRV_ld<0x9A200000, "ldidx32\t$rd, $base, $offset">;
// STIDX32/STIDX16 mask addr&~3 / addr&~1 like MEMSET32/16 — safe only while
// allowsMisalignedMemoryAccesses()==false. See the alignment warning above the
// MEMSET8/16/32 defs before enabling any unaligned store path.
def STIDX32 : RRV_st<0x9E200000, "stidx32\t$rs_data, $base, $offset">;

def LDIDX16   : RRV_ld<0x99200000, "ldidx16\t$rd, $base, $offset">   { let mayLoad = 1; }
def STIDX16   : RRV_st<0x9D200000, "stidx16\t$rs_data, $base, $offset">;
def LDIDX8    : RRV_ld<0x98200000, "ldidx8\t$rd, $base, $offset">    { let mayLoad = 1; }
def STIDX8    : RRV_st<0x9C200000, "stidx8\t$rs_data, $base, $offset">;
def LDIDX8_S  : RRV_ld<0x98A00000, "ldidx8s\t$rd, $base, $offset">   { let mayLoad = 1; }
def LDIDX16_S : RRV_ld<0x99A00000, "ldidx16s\t$rd, $base, $offset">  { let mayLoad = 1; }
// Sign-extending 32-bit indexed load (ISA v3 D1: v2 class 6 SIZE=10 SGN=1
// MODE=01, a combination the v2 RTL already implements).
def LDIDX32_S : RRV_ld<0x9AA00000, "ldidx32s\t$rd, $base, $offset">  { let mayLoad = 1; }

// Accept the canonical opcode_select.vh spelling of the signed sub-word loads
// (LDIDX8_S / LDIDX16_S) as assembler aliases for the printed mnemonics.
// Mnemonics reach the matcher lower-cased, so the alias keys are lower-case.
def : MnemonicAlias<"ldidx8_s",  "ldidx8s">;
def : MnemonicAlias<"ldidx16_s", "ldidx16s">;
def : MnemonicAlias<"ldidx32_s", "ldidx32s">;

//===----------------------------------------------------------------------===//
// Load / store isel patterns
//
// FrameIndex-based loads/stores are handled in KlaussCPUDAGToDAGISel::Select()
// (C++ code), because the TargetFrameIndex operand type (i64 after promotion)
// cannot be matched directly against a GPR in a tablegen pattern without a
// ComplexPattern.  These tablegen patterns cover the general register+offset
// case where the address is already in a GPR.
//
// simm32_imm: matches a 64-bit immediate that fits in a signed 32-bit field,
// used as the offset in LDIDX64 / STIDX64.
//===----------------------------------------------------------------------===//

def simm32_imm : ImmLeaf<i64, [{return isInt<32>(Imm);}]>;

// Materialize a signed 32-bit constant into a GPR using SETR.
// Large 64-bit constants (outside simm32) use SETR64 in Select().
def : Pat<(i64 simm32_imm:$imm), (SETR simm32_imm:$imm)>;

// Register + signed-32-bit constant addition → ADDI (one instruction).
def : Pat<(i64 (add GPR:$rs, simm32_imm:$imm)), (ADDI GPR:$rs, simm32_imm:$imm)>;

//===----------------------------------------------------------------------===//
// Immediate-form ALU patterns
//
// These fold a constant operand directly into the RV_imm / ShiftImmN
// encoding, avoiding the separate SETR (8 B) that the register-register form
// would need to materialise the constant.  This matters disproportionately on
// KlaussCPU because the core is fetch-bound — fewer fetched words ≈ fewer
// cycles.
//
// Extension semantics come straight from opcode_select.vh and gate each
// pattern — getting these wrong would silently miscompile:
//   SHLV/SHRV/SHRAV : shift count is imm[5:0]            → uimm6   (0–63)
//   ANDV/ORV/XORV   : imm32 is ZERO-extended to 64 bits  → uimm32_imm (hi32==0)
//   MULV            : imm32 is SIGN-extended to 64 bits   → simm32_imm
// Out-of-range constants simply fall through to the register-register form.
//===----------------------------------------------------------------------===//

def uimm32_imm : ImmLeaf<i64, [{ return isUInt<32>(Imm); }]>;

// Shift by a constant amount (0–63).
def : Pat<(shl GPR:$rs, uimm6:$sh), (SHLV  GPR:$rs, uimm6:$sh)>;
def : Pat<(srl GPR:$rs, uimm6:$sh), (SHRV  GPR:$rs, uimm6:$sh)>;
def : Pat<(sra GPR:$rs, uimm6:$sh), (SHRAV GPR:$rs, uimm6:$sh)>;

// Bitwise op with a mask that fits in unsigned 32 bits (imm is zero-extended).
// Masks with non-zero high 32 bits fall through to SETR64 + ANDR/ORR/XORR.
def : Pat<(and GPR:$rs, uimm32_imm:$i), (ANDV GPR:$rs, uimm32_imm:$i)>;
def : Pat<(or  GPR:$rs, uimm32_imm:$i), (ORV  GPR:$rs, uimm32_imm:$i)>;
def : Pat<(xor GPR:$rs, uimm32_imm:$i), (XORV GPR:$rs, uimm32_imm:$i)>;

// Multiply by a signed-32 constant (imm is sign-extended).
def : Pat<(mul GPR:$rs, simm32_imm:$i), (MULV GPR:$rs, simm32_imm:$i)>;

// Rotate by a constant amount (0–63) → ROLV/RORV (count = imm[5:0]).
def : Pat<(rotl GPR:$rs, uimm6:$sh), (ROLV GPR:$rs, uimm6:$sh)>;
def : Pat<(rotr GPR:$rs, uimm6:$sh), (RORV GPR:$rs, uimm6:$sh)>;

// (x & 0xFFFFFFFF) is exactly a 32→64 zero-extend → single 4-byte ZEXTW.
// AddedComplexity beats the ANDV pattern above for this specific mask.
let AddedComplexity = 3 in
def : Pat<(and GPR:$rs, 0xFFFFFFFF), (ZEXTW GPR:$rs)>;

// (x & 0xFF) / (x & 0xFFFF) → single 4-byte ZEXTB/ZEXTH instead of 8-byte ANDV.
let AddedComplexity = 3 in {
def : Pat<(and GPR:$rs, 0xFF),   (ZEXTB GPR:$rs)>;
def : Pat<(and GPR:$rs, 0xFFFF), (ZEXTH GPR:$rs)>;
}

// INCR/DECR: add ±1 as a single 4-byte op that also sets zero_flag (consumed
// by the KlaussCPUFlagReuse peephole).  AddedComplexity so they beat ADDV/ADDI.
let AddedComplexity = 2 in {
def : Pat<(add GPR:$rs,  1), (INCR GPR:$rs)>;
def : Pat<(add GPR:$rs, -1), (DECR GPR:$rs)>;
}

// Shift-by-1 needs no special case under v2: SHLV/SHRV with an embedded count
// are single-word (they were 2-word in v1, which is the only reason SHLR1/SHRR1
// were preferred then).  `(shl x, 1)` / `(srl x, 1)` now fold into the general
// SHLV/SHRV patterns above; the sole delta from SHLR1/SHRR1 is that SHLV/SHRV
// also set the Z flag, which LLVM does not model.  This mirrors rotate-by-1,
// which already lowers through ROLV/RORV — byte-identical to ROLR1/RORR1 (and
// additionally updates carry).  SHLR1/SHRR1 remain as assembler-only mnemonics.

// Global addresses and external symbols are materialized in C++ Select()
// via SETR (non-call uses) to avoid the "leaf node not emitted" crash.
// Call-site uses are handled directly by the CALL_I patterns below.

// 64-bit load from a GPR base with no offset.
def : Pat<(i64 (load (i64 GPR:$base))),
          (LDIDX64 GPR:$base, 0)>;

// 64-bit load from GPR base + signed-32 immediate offset.
def : Pat<(i64 (load (add (i64 GPR:$base), simm32_imm:$off))),
          (LDIDX64 GPR:$base, simm32_imm:$off)>;

// 64-bit store to a GPR base with no offset.
def : Pat<(store (i64 GPR:$val), (i64 GPR:$base)),
          (STIDX64 GPR:$val, GPR:$base, 0)>;

// 64-bit store to GPR base + signed-32 immediate offset.
def : Pat<(store (i64 GPR:$val), (add (i64 GPR:$base), simm32_imm:$off)),
          (STIDX64 GPR:$val, GPR:$base, simm32_imm:$off)>;

// Sub-word zero-extending loads from a plain GPR address (no offset).
// Frame-index-based sub-word loads are handled in KlaussCPUDAGToDAGISel::Select()
// which emits LDIDX8/16/32 (base+offset) so eliminateFrameIndex can rewrite normally.
def : Pat<(i64 (zextloadi8  (i64 GPR:$addr))), (MEMGET8  GPR:$addr)>;
def : Pat<(i64 (zextloadi16 (i64 GPR:$addr))), (MEMGET16 GPR:$addr)>;
def : Pat<(i64 (zextloadi32 (i64 GPR:$addr))), (MEMGET32 GPR:$addr)>;
def : Pat<(i64 (extloadi8   (i64 GPR:$addr))), (MEMGET8  GPR:$addr)>;
def : Pat<(i64 (extloadi16  (i64 GPR:$addr))), (MEMGET16 GPR:$addr)>;
def : Pat<(i64 (extloadi32  (i64 GPR:$addr))), (MEMGET32 GPR:$addr)>;

// Indexed sub-word loads: GPR base + signed-32 offset (struct/array fields, frame slots).
def : Pat<(i64 (zextloadi8  (add (i64 GPR:$base), simm32_imm:$off))),
          (LDIDX8  GPR:$base, simm32_imm:$off)>;
def : Pat<(i64 (extloadi8   (add (i64 GPR:$base), simm32_imm:$off))),
          (LDIDX8  GPR:$base, simm32_imm:$off)>;
def : Pat<(i64 (zextloadi16 (add (i64 GPR:$base), simm32_imm:$off))),
          (LDIDX16 GPR:$base, simm32_imm:$off)>;
def : Pat<(i64 (extloadi16  (add (i64 GPR:$base), simm32_imm:$off))),
          (LDIDX16 GPR:$base, simm32_imm:$off)>;
def : Pat<(i64 (zextloadi32 (add (i64 GPR:$base), simm32_imm:$off))),
          (LDIDX32 GPR:$base, simm32_imm:$off)>;
def : Pat<(i64 (extloadi32  (add (i64 GPR:$base), simm32_imm:$off))),
          (LDIDX32 GPR:$base, simm32_imm:$off)>;

// Truncating stores to a plain GPR address (no offset).
def : Pat<(truncstorei8  (i64 GPR:$val), (i64 GPR:$addr)),
          (MEMSET8  GPR:$val, GPR:$addr)>;
def : Pat<(truncstorei16 (i64 GPR:$val), (i64 GPR:$addr)),
          (MEMSET16 GPR:$val, GPR:$addr)>;
def : Pat<(truncstorei32 (i64 GPR:$val), (i64 GPR:$addr)),
          (MEMSET32 GPR:$val, GPR:$addr)>;

// Indexed truncating stores: GPR base + signed-32 offset.
def : Pat<(truncstorei8  (i64 GPR:$val), (add (i64 GPR:$base), simm32_imm:$off)),
          (STIDX8  GPR:$val, GPR:$base, simm32_imm:$off)>;
def : Pat<(truncstorei16 (i64 GPR:$val), (add (i64 GPR:$base), simm32_imm:$off)),
          (STIDX16 GPR:$val, GPR:$base, simm32_imm:$off)>;
def : Pat<(truncstorei32 (i64 GPR:$val), (add (i64 GPR:$base), simm32_imm:$off)),
          (STIDX32 GPR:$val, GPR:$base, simm32_imm:$off)>;

//===----------------------------------------------------------------------===//
// Control flow: branches and calls
//===----------------------------------------------------------------------===//

def JMP     : Vbr_uncond<0xA0000000, "jmp\t$target">;

def JMPZ    : Vbr_cond<0xA0080000, "jmpz\t$target">;
def JMPNZ   : Vbr_cond<0xA00C0000, "jmpnz\t$target">;
def JMPE    : Vbr_cond<0xA0480000, "jmpe\t$target">;
def JMPNE   : Vbr_cond<0xA04C0000, "jmpne\t$target">;
def JMPC    : Vbr_cond<0xA0100000, "jmpc\t$target">;
def JMPNC   : Vbr_cond<0xA0140000, "jmpnc\t$target">;
def JMPS    : Vbr_cond<0xA0200000, "jmps\t$target">;
def JMPNS   : Vbr_cond<0xA0240000, "jmpns\t$target">;
def JMPLT   : Vbr_cond<0xA0280000, "jmplt\t$target">;
def JMPLE   : Vbr_cond<0xA0300000, "jmple\t$target">;
def JMPGT   : Vbr_cond<0xA0340000, "jmpgt\t$target">;
def JMPGE   : Vbr_cond<0xA02C0000, "jmpge\t$target">;
def JMPULT  : Vbr_cond<0xA0380000, "jmpult\t$target">;
def JMPULE  : Vbr_cond<0xA0400000, "jmpule\t$target">;
def JMPUGT  : Vbr_cond<0xA0440000, "jmpugt\t$target">;
def JMPUGE  : Vbr_cond<0xA03C0000, "jmpuge\t$target">;

// Defs: all caller-saved GPRs clobbered by the ABI (R0-R3=args, R8-R14=temps+return)
let Uses = [SP],
    Defs = [R0, R1, R2, R3, R8, R9, R10, R11, R12, R13, R14, FLAGS] in
def CALL_I  : Vcall<0xA2000000, "call\t$target">;

//===----------------------------------------------------------------------===//
// PC-relative flow control (class 8 REL=1, and LEAPC)
// target = PC_of_this_instruction + sign_ext(imm32)   (option-B convention)
// Used in PIC mode (-fPIC / -relocation-model=pic).
// A symbolic target gets FK_KlaussCPU_PCREL32 (PCRelTarget32 and friends) so
// the assembler/linker fills in (symbol_addr − instr_addr).
//===----------------------------------------------------------------------===//

// PC-relative unconditional branch
def JMPREL   : Vbr_uncond<0xA1000000, "jmprel\t$target", [],
                          PCRelTarget32>;

// PC-relative conditional branches — flag sources same as absolute counterparts
def JMPZREL   : Vbr_relcond<0xA1080000, "jmpzrel\t$target">;   // zero_flag
def JMPNZREL  : Vbr_relcond<0xA10C0000, "jmpnzrel\t$target">;  // !zero_flag
def JMPEREL   : Vbr_relcond<0xA1480000, "jmperel\t$target">;   // equal_flag
def JMPNEREL  : Vbr_relcond<0xA14C0000, "jmpnerel\t$target">;  // !equal_flag
def JMPCREL   : Vbr_relcond<0xA1100000, "jmpcrel\t$target">;   // carry_flag
def JMPNCREL  : Vbr_relcond<0xA1140000, "jmpncrel\t$target">;  // !carry_flag
def JMPSREL   : Vbr_relcond<0xA1200000, "jmpsrel\t$target">;   // sign_flag
def JMPNSREL  : Vbr_relcond<0xA1240000, "jmpnsrel\t$target">;  // !sign_flag
def JMPLTREL  : Vbr_relcond<0xA1280000, "jmpltrel\t$target">;  // less_flag
def JMPLEREL  : Vbr_relcond<0xA1300000, "jmplerel\t$target">;  // less|equal
def JMPGTREL  : Vbr_relcond<0xA1340000, "jmpgtrel\t$target">;  // !less&!equal
def JMPGEREL  : Vbr_relcond<0xA12C0000, "jmpgerel\t$target">;  // !less_flag
def JMPULTREL : Vbr_relcond<0xA1380000, "jmpultrel\t$target">; // ult_flag
def JMPULEREL : Vbr_relcond<0xA1400000, "jmpulerel\t$target">; // ult|equal
def JMPUGTREL : Vbr_relcond<0xA1440000, "jmpugtrel\t$target">; // !ult&!equal
def JMPUGEREL : Vbr_relcond<0xA13C0000, "jmpugerel\t$target">; // !ult_flag

// PC-relative direct call: push PC+8 (return addr), PC += sign_ext(imm32)
let Uses = [SP],
    Defs = [R0, R1, R2, R3, R8, R9, R10, R11, R12, R13, R14, FLAGS] in
def CALLREL : Vcall<0xA3000000, "callrel\t$target", PCRelCallTarget32>;

// LEAPC: rd = PC_of_instruction + sign_ext(imm32)  (address materialisation)
// Class 2 LEA: word 1 = simm32, FK_KlaussCPU_PCREL32 for a symbol.
let hasSideEffects = 0, isReMaterializable = 1 in
def LEAPC   : RV_ld<0x8B800000, "leapc\t$rd, $imm", [], pcrel_simm32>;

//===----------------------------------------------------------------------===//
// Special / system
//===----------------------------------------------------------------------===//

let isReturn = 1, isTerminator = 1, isBarrier = 1 in
def RET_I   : I0<0x65800000, (outs), (ins), "ret", [(KlaussCPURetGlue)]>;
def NOP_I   : I0<0x6C000000, (outs), (ins), "nop">;
def HALT_I  : I0<0x6C010000, (outs), (ins), "halt">;
def IRET_I  : I0<0x65C00000, (outs), (ins), "iret">;

// TRAP — debug breakpoint (v2 class B, OP 4: 0x6C040000).  With a debugger
// attached the core halts at the TRAP's address; otherwise it raises the
// ERR_TRAP crash dump.  Used for software breakpoints and __builtin_debugtrap.
let hasSideEffects = 1 in
def TRAP_I  : I0<0x6C040000, (outs), (ins), "trap">;

// WAIT — interruptible core-suspend: stop fetching/executing and resume when an
// unmasked, vectored interrupt becomes pending (timer tick, future wired source).
// No register/flag side effects; IRET resumes at PC+4.  Distinct from HALT (which
// stops permanently).  hasSideEffects keeps it from being reordered/eliminated.
let hasSideEffects = 1 in
def WAIT_I  : I0<0x6C020000, (outs), (ins), "wait">;

//===----------------------------------------------------------------------===//
// Peripheral I-O instructions (hasSideEffects prevents reordering/elimination)
//
// UART is memory-mapped (MMIO at 0xF001_0000) and accessed via ordinary
// volatile loads/stores — there are no UART opcodes/intrinsics anymore.
//===----------------------------------------------------------------------===//

let hasSideEffects = 1 in {
  // ── Retired v1 peripheral opcodes (LEN=00 v1 words; see RetiredV1_R)
  // Set 16 on-board LEDs from register bits [15:0].
  def LEDR_R      : RetiredV1_R<0x00003000, "ledr\t$reg">;
  // Set 7-seg display 1 from register.
  def SEG7_1R_R   : RetiredV1_R<0x00003020, "7seg1r\t$reg">;
  // Set 7-seg display 2 from register.
  def SEG7_2R_R   : RetiredV1_R<0x00003030, "7seg2r\t$reg">;
  // Set both 7-seg displays from register.
  def SEG7R_R     : RetiredV1_R<0x00003040, "7segr\t$reg">;
  // Blank both 7-seg displays.
  let isRetiredV1 = 1 in
  def SEG7BLANK_I : I0<0x00003073, (outs), (ins), "7segblank">;
}

//===----------------------------------------------------------------------===//
// Assembler-only opcodes — full opcode_select.vh coverage
//
// These instructions are NOT emitted by the code generator (no isel Pattern);
// they exist so hand-written `.s` / inline-asm can name every hardware opcode
// in opcode_select.vh.  Their word-0 templates are from ISA_ENCODING_V2_MAP,
// so the bytes match the hardware-verified `klausscc` assembler.
//===----------------------------------------------------------------------===//

// ── RRR (rd, rs1, rs2) ─────────────────────────────────────────────────────
def ADDC   : RRR<0x44A00000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "addc\t$rd, $rs1, $rs2"> { let Uses = [FLAGS]; } // reads carry-in
def SUBC   : RRR<0x44E00000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "subc\t$rd, $rs1, $rs2"> { let Uses = [FLAGS]; } // reads borrow-in
def BSETRR : RRR<0x52000000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "bsetrr\t$rd, $rs1, $rs2">;
def BCLRRR : RRR<0x52400000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "bclrrr\t$rd, $rs1, $rs2">;
def BTGLRR : RRR<0x52800000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "btglrr\t$rd, $rs1, $rs2">;
def BTSTRR : RRR<0x52C00000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "btstrr\t$rd, $rs1, $rs2">;

// ── R in-place (rd = f(rd); 2-address) ─────────────────────────────────────
def SETFR : R_inplace<0x57000000, "setfr\t$rd">;
// Same encoding as SHLR1, which is the spelling the disassembler prints.
let isAsmParserOnly = 1 in
def SHLAR : R_unary<0x50208000, "shlar">;
def SHRAR : R_unary<0x50A08000, "shrar">;
def ROLR1 : R_unary<0x50E0C000, "rolr1">;
def RORR1 : R_unary<0x5120C000, "rorr1">;
def ROLCR : R_unary<0x5160C000, "rolcr"> { let Uses = [FLAGS]; } // rotate through carry
def RORCR : R_unary<0x51A0C000, "rorcr"> { let Uses = [FLAGS]; } // rotate through carry

// ── Bit ops by embedded position N, bit-field extract / deposit ────────────
// Bit set/clear/toggle by embedded position N — v2: single word.
def BSET  : ShiftImmN<0x52200000, "bset">;
def BCLR  : ShiftImmN<0x52600000, "bclr">;
def BTGL  : ShiftImmN<0x52A00000, "btgl">;
// Bit-field extract / deposit: 2 words, the field descriptor in imm32
// (start [4:0], len [12:8]; 32-bit fields, result's upper half zero).
//   BEXTR rd, rs1, imm       rd = (rs1 >> start) & mask(len)   (sets Z)
//   BDEP  rd, rs1, rs2, imm  rd = rs1 with rs2[len-1:0] deposited at start
// BEXTR's rs2 field must be 0 (the template's), which the CPU checks.
def BEXTR : RV_imm<0x93000000, "bextr", uimm32>;
def BDEP  : RRRV<0x93400000, "bdep", uimm32>;
// Bit-test by embedded position N — v2: single word, flag-only.
def BTST  : BitTestImmN<0x52E00000, "btst\t$rs, $imm">;

// ── RR (rd, rs) load / (rs_data, rs_addr) store — 8-byte aligned ────────────
def MEMREADRR  : RR1<0x5B000000, (outs GPR:$rd), (ins GPR:$rs), "memreadrr\t$rd, $rs"> { let mayLoad = 1; }
def MEMSET64RR : RRst<0x5F000000, "memset64rr\t$rs_data, $rs_addr">;

// ── RV absolute memory access (rd/rs, address-imm32) — symbol-capable ───────
def MEMREADR : RV_ld<0x9B400000, "memreadr\t$rd, $imm", [], uimm32_abs> { let mayLoad = 1; }
def MEMSETR  : RV_st<0x9F400000, "memsetr\t$rs_data, $addr"> { let mayStore = 1; }

// ── RRV 64-bit indexed: non-aligned (vh LDIDX64/STIDX64 = 0x0C/0x0D) and
//    register-offset (vh LDIDX64R/STIDX64R).  NOTE: LLVM's ldidx64/stidx64 are
//    the *force-aligned* 0xFC/0xFD (= vh LDIDX64A/STIDX64A); aliases below.
//    For the *R variants the offset is a second register (EA = base + offset).
def LDIDX64U : RRV_ld<0x9B200000, "ldidx64u\t$rd, $base, $offset">;
def STIDX64U : RRV_st<0x9F200000, "stidx64u\t$rs_data, $base, $offset">;
// v2: register-offset indexed ops are a single word (offset reg in rs2[3:0]).
def LDIDX64R : Idx64RegOff_ld<0x5B600000, "ldidx64r\t$rd, $base, $offset">;
def STIDX64R : Idx64RegOff_st<0x5F600000, "stidx64r\t$rs_data, $base, $offset">;

// ── V (fixed op32 + imm32/address) — push / conditional jumps ───────────────
def PUSHV : Vsp<0xA4400000, "pushv\t$imm", uimm32> { let mayStore = 1; }
def JMPO  : Vbr_cond<0xA0180000, "jmpo\t$target">;
def JMPNO : Vbr_cond<0xA01C0000, "jmpno\t$target">;

// ── Vcall (conditional calls) ───────────────────────────────────────────────
def CALLZ  : Vcall<0xA2080000, "callz\t$target">;
def CALLNZ : Vcall<0xA20C0000, "callnz\t$target">;
def CALLE  : Vcall<0xA2480000, "calle\t$target">;
def CALLNE : Vcall<0xA24C0000, "callne\t$target">;
def CALLC  : Vcall<0xA2100000, "callc\t$target">;
def CALLNC : Vcall<0xA2140000, "callnc\t$target">;
def CALLO  : Vcall<0xA2180000, "callo\t$target">;
def CALLNO : Vcall<0xA21C0000, "callno\t$target">;

// ── V64 (push 64-bit immediate; 12-byte) ────────────────────────────────────
def PUSHV64 : V64_imm<0xE4400000, "pushv64\t$lo, $hi"> { let mayStore = 1; }

// ── Peripheral / timing ops (side-effecting) ────────────────────────────────
let hasSideEffects = 1 in {
  // R_in (single source register)
  def DELAYR   : R_in<0x6C050000, (ins GPR:$reg), "delayr\t$reg">;
  def RGB1R    : RetiredV1_R<0x00003050, "rgb1r\t$reg">;   // retired v1
  def RGB2R    : RetiredV1_R<0x00003060, "rgb2r\t$reg">;   // retired v1
  def LCDDATAR : R_in<0x71000000, (ins GPR:$reg), "lcddatar\t$reg">;
  // V (fixed op32 + imm32)
  def DELAYV   : Vsp<0xAC050000, "delayv\t$imm", imm32>;
  let isRetiredV1 = 1 in {   // v1 words (LEN=00), see RetiredV1_R
  def LEDV     : Vsp<0x00003070, "ledv\t$imm", imm32>;
  def RGB1V    : Vsp<0x00003074, "rgb1v\t$imm", imm32>;
  def RGB2V    : Vsp<0x00003075, "rgb2v\t$imm", imm32>;
  }
  def LCD      : Vsp<0xB2000000, "lcd\t$imm", imm32>;
  def LCDCMDV  : Vsp<0xB0000000, "lcdcmdv\t$imm", imm32>;
  def LCDDATAV : Vsp<0xB1000000, "lcddatav\t$imm", imm32>;
}

// ── Mnemonic aliases for the canonical opcode_select.vh spellings ───────────
// 0xFC/0xFD are the force-aligned 64-bit indexed ops (= vh LDIDX64A/STIDX64A);
// LLVM already defines them as ldidx64/stidx64.
def : MnemonicAlias<"ldidx64a", "ldidx64">;
def : MnemonicAlias<"stidx64a", "stidx64">;

//===----------------------------------------------------------------------===//
// Call patterns — matched from KlaussCPUISD::CALL nodes emitted by LowerCall.
//===----------------------------------------------------------------------===//

// Direct call to a global address or external symbol (non-PIC mode).
// PIC direct calls (CALLREL) are intercepted in KlaussCPUDAGToDAGISel::Select()
// before SelectCode() reaches these patterns.
// Callee is i64 (global address zero-extended into a GPR; encoder takes [31:0]).
def : Pat<(KlaussCPUCallNode (i64 tglobaladdr:$func)),
          (CALL_I tglobaladdr:$func)>;
def : Pat<(KlaussCPUCallNode (i64 texternalsym:$func)),
          (CALL_I texternalsym:$func)>;

// Indirect call via register (function pointer).
def : Pat<(KlaussCPUCallNode (i64 GPR:$func)),
          (CALLR_R GPR:$func)>;

// Sign-extending byte/halfword loads — pure GPR address (no base+offset needed).
def : Pat<(i64 (sextloadi8  (i64 GPR:$addr))), (LDIDX8_S  GPR:$addr, 0)>;
def : Pat<(i64 (sextloadi16 (i64 GPR:$addr))), (LDIDX16_S GPR:$addr, 0)>;

// Sign-extending byte/halfword loads — GPR base + signed-32 offset.
def : Pat<(i64 (sextloadi8  (add (i64 GPR:$base), simm32_imm:$off))),
          (LDIDX8_S  GPR:$base, simm32_imm:$off)>;
def : Pat<(i64 (sextloadi16 (add (i64 GPR:$base), simm32_imm:$off))),
          (LDIDX16_S GPR:$base, simm32_imm:$off)>;

// ISA v3 D1: sign-extending 32-bit load (replaces LDIDX32 + SEXTW).
def : Pat<(i64 (sextloadi32 (i64 GPR:$addr))), (LDIDX32_S GPR:$addr, 0)>;
def : Pat<(i64 (sextloadi32 (add (i64 GPR:$base), simm32_imm:$off))),
          (LDIDX32_S GPR:$base, simm32_imm:$off)>;

//===----------------------------------------------------------------------===//
// ISA v3 short 1-word forms (ISA_V3_PROPOSAL.md §3, A3/A4/A5).
//
// Never selected by isel. KlaussCPUMCCompress (MCTargetDesc) rewrites the
// 2-word form into its short twin at MC emission — from the AsmPrinter and
// the AsmParser alike — when the immediate fits the 8-bit word-0 field
// (loads/stores: scaled by the access size). Operand lists and asm text match
// the long form exactly, so the rewrite is an opcode swap.
// -mllvm -klausscpu-short-forms=false disables it.
//
// Encoding (ISA_ENCODING_V2_MAP §7.1): the long twin's word 0 with LEN=01
// and the 8-bit immediate in [19:12]; a load/store offset is stored divided
// by the access size (1 << shift).
//===----------------------------------------------------------------------===//

// The imm8 operands: signed or unsigned as the long twin extends its imm32.
// A load/store offset is a byte offset: simm8_lslN is bits [N+7:N] of an
// (N+8)-bit signed offset whose low N bits are zero.
class ShortImmOperand<string decoder> : Operand<i64> {
  let DecoderMethod = decoder;
}
def uimm8 : ShortImmOperand<"decodeUImmOperand<8>">;
foreach shift = 0-3 in
def simm8_lsl # shift
    : ShortImmOperand<"decodeSImmOperand<" # !add(8, shift) # ">">;

// The short forms are decoded and printed like any instruction, but the
// assembler never matches them (their asm string is the long form's, which
// the matcher picks): AsmVariantName names no parser variant.
class ShortForm<KCInst64 long, dag outs, dag ins, string asmstr>
    : KCInst32<{0b01, long.Word0{29-0}}, outs, ins, asmstr> {
  let AsmVariantName = "NoMatch";
}

// rd = imm8 (SETR)
class ShortRdImm<KCInst64 long, string asmstr>
    : ShortForm<long, (outs GPR:$rd), (ins simm8_lsl0:$imm), asmstr> {
  bits<4> rd;
  bits<8> imm;
  let Inst{19-12} = imm;
  let Inst{11-8}  = rd;
}

// rd = rs op imm8
class ShortRdRsImm<KCInst64 long, string asmstr, Operand immop = simm8_lsl0>
    : ShortForm<long, (outs GPR:$rd), (ins GPR:$rs, immop:$imm), asmstr> {
  bits<4> rd;
  bits<4> rs;
  bits<8> imm;
  let Inst{19-12} = imm;
  let Inst{11-8}  = rd;
  let Inst{7-4}   = rs;
}

// Compare rs with imm8 (flags only)
class ShortRsImm<KCInst64 long, string asmstr>
    : ShortForm<long, (outs), (ins GPR:$rs, simm8_lsl0:$imm), asmstr> {
  bits<4> rs;
  bits<8> imm;
  let Inst{19-12} = imm;
  let Inst{7-4}   = rs;
}

class ShortLoad<KCInst64 long, int shift, string asmstr>
    : ShortForm<long, (outs GPR:$rd),
                (ins GPR:$base, !cast<Operand>("simm8_lsl" # shift):$offset),
                asmstr> {
  bits<4>  rd;
  bits<4>  base;
  bits<32> offset;
  // Bits [shift+7 : shift] of the byte offset: the scaled imm8.
  let Inst{19-12} = !cond(!eq(shift, 0): offset{7-0},
                          !eq(shift, 1): offset{8-1},
                          !eq(shift, 2): offset{9-2},
                          !eq(shift, 3): offset{10-3});
  let Inst{11-8}  = rd;
  let Inst{7-4}   = base;
}

class ShortStore<KCInst64 long, int shift, string asmstr>
    : ShortForm<long, (outs),
                (ins GPR:$rs_data, GPR:$base,
                     !cast<Operand>("simm8_lsl" # shift):$offset),
                asmstr> {
  bits<4>  rs_data;
  bits<4>  base;
  bits<32> offset;
  // Bits [shift+7 : shift] of the byte offset: the scaled imm8.
  let Inst{19-12} = !cond(!eq(shift, 0): offset{7-0},
                          !eq(shift, 1): offset{8-1},
                          !eq(shift, 2): offset{9-2},
                          !eq(shift, 3): offset{10-3});
  let Inst{11-8}  = rs_data;
  let Inst{7-4}   = base;
}

// A1 short PC-relative branch: the absolute 2-word branch's word 0 with
// LEN=01 and REL=1 [24], keeping COND/INV [22:18]; simm18 word displacement
// in [17:0].
class ShortBranch<KCInst64 long, string asmstr>
    : KCInst32<{0b01, long.Word0{29-25}, 0b1, long.Word0{23-18},
                0b000000000000000000},
               (outs), (ins PCRelTarget18:$target), asmstr> {
  bits<20> target;
  let Inst{17-0} = target{19-2};
  let AsmVariantName = "NoMatch";
}

let hasSideEffects = 0 in {
def SETR_SH    : ShortRdImm<SETR, "setr\t$rd, $imm">;
let Defs = [FLAGS] in {
def ADDV_SH    : ShortRdRsImm<ADDV, "addv\t$rd, $rs, $imm", uimm8>;
def MINUSV_SH  : ShortRdRsImm<MINUSV, "minusv\t$rd, $rs, $imm", uimm8>;
def ANDV_SH    : ShortRdRsImm<ANDV, "andv\t$rd, $rs, $imm", uimm8>;
def ORV_SH     : ShortRdRsImm<ORV, "orv\t$rd, $rs, $imm", uimm8>;
def XORV_SH    : ShortRdRsImm<XORV, "xorv\t$rd, $rs, $imm", uimm8>;
def ADDI_SH    : ShortRdRsImm<ADDI, "addi\t$rd, $rs, $imm">;
def CMPRV_SH   : ShortRsImm<CMPRV_I, "cmprv\t$rs, $imm">;
}
let mayLoad = 1 in {
def LDIDX8_SH    : ShortLoad<LDIDX8, 0, "ldidx8\t$rd, $base, $offset">;
def LDIDX8_S_SH  : ShortLoad<LDIDX8_S, 0, "ldidx8s\t$rd, $base, $offset">;
def LDIDX16_SH   : ShortLoad<LDIDX16, 1, "ldidx16\t$rd, $base, $offset">;
def LDIDX16_S_SH : ShortLoad<LDIDX16_S, 1, "ldidx16s\t$rd, $base, $offset">;
def LDIDX32_SH   : ShortLoad<LDIDX32, 2, "ldidx32\t$rd, $base, $offset">;
def LDIDX32_S_SH : ShortLoad<LDIDX32_S, 2, "ldidx32s\t$rd, $base, $offset">;
def LDIDX64_SH   : ShortLoad<LDIDX64, 3, "ldidx64\t$rd, $base, $offset">;
def LDIDX64U_SH  : ShortLoad<LDIDX64U, 3, "ldidx64u\t$rd, $base, $offset">;
}
// A1 short PC-relative branches (1 word, simm18 words). Produced by the same
// compressor for any branch to a symbolic target and relaxed back to the
// 2-word PC-relative form by the asm backend when out of range / not local.
let isBranch = 1, isTerminator = 1, Uses = [FLAGS] in {
foreach c = ["z", "nz", "c", "nc", "o", "no", "s", "ns", "lt", "ge", "le",
             "gt", "ult", "uge", "ule", "ugt", "e", "ne"] in
  def JMP # !toupper(c) # _SH
      : ShortBranch<!cast<KCInst64>("JMP" # !toupper(c)),
                    // O/NO have no 2-word PC-relative spelling; print the
                    // absolute one (the target address is the same, and it
                    // reassembles to this op).
                    !if(!or(!eq(c, "o"), !eq(c, "no")), "jmp" # c,
                        "jmp" # c # "rel") # "\t$target">;
}
let isBranch = 1, isTerminator = 1, isBarrier = 1 in
def JMP_SH : ShortBranch<JMP, "jmprel\t$target">;

let mayStore = 1 in {
def STIDX8_SH    : ShortStore<STIDX8, 0, "stidx8\t$rs_data, $base, $offset">;
def STIDX16_SH   : ShortStore<STIDX16, 1, "stidx16\t$rs_data, $base, $offset">;
def STIDX32_SH   : ShortStore<STIDX32, 2, "stidx32\t$rs_data, $base, $offset">;
def STIDX64_SH   : ShortStore<STIDX64, 3, "stidx64\t$rs_data, $base, $offset">;
def STIDX64U_SH  : ShortStore<STIDX64U, 3, "stidx64u\t$rs_data, $base, $offset">;
}
}

// ISA v3 B: fused compare-and-branch (class 0xD, 1 word, flags untouched).
// $cc = PRED (0 EQ, 1 LT, 2 LE, 3 ULT, 4 ULE) | INV<<3; simm13 word
// displacement via FK_KlaussCPU_PCREL13. Built only by KlaussCPUFuseCmpBr.
// Word 0: PRED [25:23], INV [22], IMM [21], disp13 [20:8], rs1 [7:4],
// rs2 or simm4 [3:0].
class FusedBranch<bits<32> word0, dag ins, string asmstr>
    : KCInst32<word0, (outs), ins, asmstr> {
  bits<4>  rs1;
  bits<4>  cc;
  bits<15> target;
  let Inst{25-23} = cc{2-0};
  let Inst{22}    = cc{3};
  let Inst{20-8}  = target{14-2};
  let Inst{7-4}   = rs1;
}

let isBranch = 1, isTerminator = 1, hasSideEffects = 0 in {
def FBR_RR : FusedBranch<0x74000000,
                         (ins GPR:$rs1, GPR:$rs2, fbr_cc:$cc,
                              PCRelTarget13:$target),
                         "fbr\t$rs1, $rs2, $cc, $target"> {
  bits<4> rs2;
  let Inst{3-0} = rs2;
}
def FBR_RI : FusedBranch<0x74200000,
                         (ins GPR:$rs1, fbr_simm4:$imm, fbr_cc:$cc,
                              PCRelTarget13:$target),
                         "fbri\t$rs1, $imm, $cc, $target"> {
  bits<4> imm;
  let Inst{3-0} = imm;
}
}

//===----------------------------------------------------------------------===//
// Legacy 2-address assembler spellings.  The ops above are 3-operand in v2
// (rd, rs, ...).  Keep the old in-place forms (`incr r1`, `addv r1, 5`,
// `shlv r1, 3`) assembling as rd == rs so existing hand-written .s / inline
// asm keeps working.  The printer always emits the 3-operand form.
//===----------------------------------------------------------------------===//
def : InstAlias<"shlv\t$rd, $imm", (SHLV GPR:$rd, GPR:$rd, uimm6:$imm)>;
def : InstAlias<"shrv\t$rd, $imm", (SHRV GPR:$rd, GPR:$rd, uimm6:$imm)>;
def : InstAlias<"shrav\t$rd, $imm", (SHRAV GPR:$rd, GPR:$rd, uimm6:$imm)>;
def : InstAlias<"rolv\t$rd, $imm", (ROLV GPR:$rd, GPR:$rd, uimm6:$imm)>;
def : InstAlias<"rorv\t$rd, $imm", (RORV GPR:$rd, GPR:$rd, uimm6:$imm)>;
def : InstAlias<"bset\t$rd, $imm", (BSET GPR:$rd, GPR:$rd, uimm6:$imm)>;
def : InstAlias<"bclr\t$rd, $imm", (BCLR GPR:$rd, GPR:$rd, uimm6:$imm)>;
def : InstAlias<"btgl\t$rd, $imm", (BTGL GPR:$rd, GPR:$rd, uimm6:$imm)>;
def : InstAlias<"incr\t$rd", (INCR GPR:$rd, GPR:$rd)>;
def : InstAlias<"decr\t$rd", (DECR GPR:$rd, GPR:$rd)>;
def : InstAlias<"negr\t$rd", (NEGR GPR:$rd, GPR:$rd)>;
def : InstAlias<"absr\t$rd", (ABSR GPR:$rd, GPR:$rd)>;
def : InstAlias<"shlr1\t$rd", (SHLR1 GPR:$rd, GPR:$rd)>;
def : InstAlias<"shrr1\t$rd", (SHRR1 GPR:$rd, GPR:$rd)>;
def : InstAlias<"notr\t$rd", (NOTR GPR:$rd, GPR:$rd)>;
def : InstAlias<"popcnt\t$rd", (POPCNT GPR:$rd, GPR:$rd)>;
def : InstAlias<"clz\t$rd", (CLZ GPR:$rd, GPR:$rd)>;
def : InstAlias<"ctz\t$rd", (CTZ GPR:$rd, GPR:$rd)>;
def : InstAlias<"bitrev\t$rd", (BITREV GPR:$rd, GPR:$rd)>;
def : InstAlias<"bswap\t$rd", (BSWAP_R GPR:$rd, GPR:$rd)>;
def : InstAlias<"sextb\t$rd", (SEXTB GPR:$rd, GPR:$rd)>;
def : InstAlias<"sexth\t$rd", (SEXTH GPR:$rd, GPR:$rd)>;
def : InstAlias<"zextb\t$rd", (ZEXTB GPR:$rd, GPR:$rd)>;
def : InstAlias<"zexth\t$rd", (ZEXTH GPR:$rd, GPR:$rd)>;
def : InstAlias<"sextw\t$rd", (SEXTW GPR:$rd, GPR:$rd)>;
def : InstAlias<"zextw\t$rd", (ZEXTW GPR:$rd, GPR:$rd)>;
def : InstAlias<"shlar\t$rd", (SHLAR GPR:$rd, GPR:$rd)>;
def : InstAlias<"shrar\t$rd", (SHRAR GPR:$rd, GPR:$rd)>;
def : InstAlias<"rolr1\t$rd", (ROLR1 GPR:$rd, GPR:$rd)>;
def : InstAlias<"rorr1\t$rd", (RORR1 GPR:$rd, GPR:$rd)>;
def : InstAlias<"rolcr\t$rd", (ROLCR GPR:$rd, GPR:$rd)>;
def : InstAlias<"rorcr\t$rd", (RORCR GPR:$rd, GPR:$rd)>;
def : InstAlias<"addv\t$rd, $imm", (ADDV GPR:$rd, GPR:$rd, uimm32:$imm)>;
def : InstAlias<"minusv\t$rd, $imm", (MINUSV GPR:$rd, GPR:$rd, uimm32:$imm)>;
def : InstAlias<"andv\t$rd, $imm", (ANDV GPR:$rd, GPR:$rd, uimm32:$imm)>;
def : InstAlias<"orv\t$rd, $imm", (ORV GPR:$rd, GPR:$rd, uimm32:$imm)>;
def : InstAlias<"xorv\t$rd, $imm", (XORV GPR:$rd, GPR:$rd, uimm32:$imm)>;
def : InstAlias<"mulv\t$rd, $imm", (MULV GPR:$rd, GPR:$rd, simm32:$imm)>;
def : InstAlias<"divv\t$rd, $imm", (DIVV GPR:$rd, GPR:$rd, simm32:$imm)>;
def : InstAlias<"modv\t$rd, $imm", (MODV GPR:$rd, GPR:$rd, simm32:$imm)>;
def : InstAlias<"bextr\t$rd, $imm", (BEXTR GPR:$rd, GPR:$rd, uimm32:$imm)>;
// The old `bdep rd, imm` had no source operand (it deposited r0) and has no
// alias: write `bdep rd, rd, rs, imm`.

//===----------------------------------------------------------------------===//
// Bit-manipulation-by-constant patterns (need BSET/BCLR/BTGL defined above)
//===----------------------------------------------------------------------===//

// Single-bit set / clear / toggle → 4-byte BSET/BCLR/BTGL with the bit
// position embedded (word0[20:15]).  Beats the 8-byte ORV/ANDV/XORV for bits
// 0-31, and the 12-byte SETR64 + 4-byte reg-reg op for bits 32-63.
def BitPosXForm : SDNodeXForm<imm, [{
  return CurDAG->getTargetConstant(
      llvm::countr_zero(static_cast<uint64_t>(N->getZExtValue())), SDLoc(N),
      MVT::i64);
}]>;
def ClearBitPosXForm : SDNodeXForm<imm, [{
  return CurDAG->getTargetConstant(
      llvm::countr_zero(~static_cast<uint64_t>(N->getZExtValue())), SDLoc(N),
      MVT::i64);
}]>;
def SingleBitImm : ImmLeaf<i64, [{
  return llvm::isPowerOf2_64(static_cast<uint64_t>(Imm));
}]>;
def SingleZeroBitImm : ImmLeaf<i64, [{
  return llvm::isPowerOf2_64(~static_cast<uint64_t>(Imm));
}]>;
let AddedComplexity = 4 in {
def : Pat<(or  GPR:$rs, SingleBitImm:$b),
          (BSET GPR:$rs, (BitPosXForm imm:$b))>;
def : Pat<(xor GPR:$rs, SingleBitImm:$b),
          (BTGL GPR:$rs, (BitPosXForm imm:$b))>;
def : Pat<(and GPR:$rs, SingleZeroBitImm:$b),
          (BCLR GPR:$rs, (ClearBitPosXForm imm:$b))>;
}

// 64-bit load/store at (base + index register) → 1-word LDIDX64R/STIDX64R
// instead of ADDR + 2-word LDIDX64/STIDX64 (e.g. `p[i]` after the index
// shift).  EA is not force-aligned (A=0), which is fine: LLVM never emits an
// under-aligned i64 access (allowsMisalignedMemoryAccesses() is false).
def : Pat<(i64 (load (add (i64 GPR:$base), (i64 GPR:$idx)))),
          (LDIDX64R GPR:$base, GPR:$idx)>;
def : Pat<(store (i64 GPR:$val), (add (i64 GPR:$base), (i64 GPR:$idx))),
          (STIDX64R GPR:$val, GPR:$base, GPR:$idx)>;
