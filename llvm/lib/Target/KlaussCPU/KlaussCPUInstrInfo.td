//===-- KlaussCPUInstrInfo.td - KlaussCPU Instruction Definitions ----------===//
//
// NOTE ON ENCODING (ISA v2, the flag-day renumbering — ISA_ENCODING_V2.md):
// The **authoritative** instruction bytes are produced by the hand-written
// KlaussCPUMCCodeEmitter.cpp (there is no -gen-emitter / -gen-disassembler).
// The opcode template arguments and `let Inst{}` fields in THIS file are
// legacy/documentary only and are NOT consumed by any generated code.  What
// this file must get right for v2 is each instruction's **Size**, operand
// list, and 2-address ties — those drive register allocation and the emitter's
// operand indexing.  Do not trust the numeric `op` args below as v2 opcodes.
//
// v2 word-0 layout (all instructions, little-endian on the bus):
//   [31:30]=LEN(01/10/11 = 1/2/3 words)  [29:26]=CLASS  [25:12]=attrs+OP
//   [11:8]=rd  [7:4]=rs1  [3:0]=rs2   (stores put their data source in rd)
//
//   Legacy format table (v1 bit layout; kept for the class helpers below):
//   Format   Size  Layout
//   ------   ----  ------
//   RRR       4B   [31:12]=op20  [11:8]=rd  [7:4]=rs1  [3:0]=rs2
//   RR        4B   [31:8]=op24   [7:4]=rd   [3:0]=rs
//   R         4B   [31:4]=op28   [3:0]=reg
//   I0        4B   [31:0]=exact_opcode
//   RV        8B   [63:36]=op28  [35:32]=rd  [31:0]=imm32
//   RRV       8B   [63:40]=op24  [39:36]=rd  [35:32]=rs  [31:0]=offset32
//   Vimm      8B   [63:32]=exact_opcode32    [31:0]=target32
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
// Used by the AsmParser to validate and render parsed operands.
//===----------------------------------------------------------------------===//

// Matches any immediate expression (integer literal or label).
def AnyImmAsmOperand : AsmOperandClass {
  let Name            = "AnyImm";
  let PredicateMethod = "isImm";
  let RenderMethod    = "addImmOperands";
}

def BrTargetAsmOperand : AsmOperandClass {
  let Name            = "BrTarget";
  let PredicateMethod = "isImm";
  let RenderMethod    = "addImmOperands";
}

//===----------------------------------------------------------------------===//
// Operand types
//===----------------------------------------------------------------------===//

def simm32 : Operand<i64> {
  let DecoderMethod    = "decodeSimm32";
  let ParserMatchClass = AnyImmAsmOperand;
}

def uimm32 : Operand<i64> {
  let DecoderMethod    = "decodeUimm32";
  let ParserMatchClass = AnyImmAsmOperand;
}

// 32-bit absolute branch/call target address
def BrTarget32 : Operand<OtherVT> {
  let EncoderMethod    = "encodeBrTarget";
  let ParserMatchClass = BrTargetAsmOperand;
}

def CallTarget32 : Operand<i64> {
  let EncoderMethod    = "encodeBrTarget";
  let ParserMatchClass = BrTargetAsmOperand;
}

//===----------------------------------------------------------------------===//
// Instruction base classes
//===----------------------------------------------------------------------===//

class KCInst32<dag outs, dag ins, string asmstr, list<dag> pat = []>
    : Instruction {
  let Namespace        = "KlaussCPU";
  let Size             = 4;
  dag OutOperandList   = outs;
  dag InOperandList    = ins;
  let AsmString        = asmstr;
  let Pattern          = pat;
  field bits<32> Inst;
  field bits<32> SoftFail = 0;
  let DecoderNamespace = "KlaussCPU";
  let SchedRW          = [WriteALU];   // default; overridden per format (M8)
}

class KCInst64<dag outs, dag ins, string asmstr, list<dag> pat = []>
    : Instruction {
  let Namespace        = "KlaussCPU";
  let Size             = 8;
  dag OutOperandList   = outs;
  dag InOperandList    = ins;
  let AsmString        = asmstr;
  let Pattern          = pat;
  field bits<64> Inst;
  field bits<64> SoftFail = 0;
  let DecoderNamespace = "KlaussCPU";
  let SchedRW          = [WriteALU];   // default; overridden per format (M8)
}

// 12-byte (3-word) instruction: opcode word + lo32 + hi32.
// Used for SETR64.  KlaussCPUMCCodeEmitter handles this opcode manually
// before consulting any tablegen-generated emitter table, so bits<96>
// never overflows the uint64_t return type of getBinaryCodeForInstr().
class KCInst96<dag outs, dag ins, string asmstr, list<dag> pat = []>
    : Instruction {
  let Namespace        = "KlaussCPU";
  let Size             = 12;
  dag OutOperandList   = outs;
  dag InOperandList    = ins;
  let AsmString        = asmstr;
  let Pattern          = pat;
  field bits<96> Inst;
  field bits<96> SoftFail = 0;
  let DecoderNamespace = "KlaussCPU";
  let SchedRW          = [WriteALU];   // default; overridden per format (M8)
}

//===----------------------------------------------------------------------===//
// Format: RRR  rd = rs1 op rs2
// [31:12]=op20  [11:8]=rd  [7:4]=rs1  [3:0]=rs2
//===----------------------------------------------------------------------===//

class RRR<bits<20> op, dag outs, dag ins, string asmstr, list<dag> pat = []>
    : KCInst32<outs, ins, asmstr, pat> {
  bits<4> rd;
  bits<4> rs1;
  bits<4> rs2;
  let Inst{31-12} = op;
  let Inst{11-8}  = rd;
  let Inst{7-4}   = rs1;
  let Inst{3-0}   = rs2;
  let Defs        = [FLAGS];   // arith/cmp/shift/mul/div set flags (bitwise over-marked; safe)
}

//===----------------------------------------------------------------------===//
// Format: RR   two-register
// [31:8]=op24  [7:4]=rd  [3:0]=rs
//===----------------------------------------------------------------------===//

// 1 output, 1 input (COPY, MEMGET64)
class RR1<bits<24> op, dag outs, dag ins, string asmstr, list<dag> pat = []>
    : KCInst32<outs, ins, asmstr, pat> {
  bits<4> rd;
  bits<4> rs;
  let Inst{31-8} = op;
  let Inst{7-4}  = rd;
  let Inst{3-0}  = rs;
  let SchedRW    = [WriteLoad];   // MEMGET*/MEMREADRR (COPY_R overrides to WriteALU)
}

// 0 outputs, 2 source inputs (CMPRR)
class RR0<bits<24> op, string asmstr, list<dag> pat = []>
    : KCInst32<(outs), (ins GPR:$rs1, GPR:$rs2), asmstr, pat> {
  bits<4> rs1;
  bits<4> rs2;
  let Inst{31-8} = op;
  let Inst{7-4}  = rs1;
  let Inst{3-0}  = rs2;
  let Defs       = [FLAGS];    // CMPRR sets flags
}

// Store: data in [7:4], address in [3:0], no output (MEMSET64)
class RRst<bits<24> op, string asmstr, list<dag> pat = []>
    : KCInst32<(outs), (ins GPR:$rs_data, GPR:$rs_addr), asmstr, pat> {
  bits<4> rs_data;
  bits<4> rs_addr;
  let Inst{31-8} = op;
  let Inst{7-4}  = rs_data;
  let Inst{3-0}  = rs_addr;
  let mayStore   = 1;
  let SchedRW    = [WriteStore];
}

//===----------------------------------------------------------------------===//
// Format: R   single-register
// [31:4]=op28  [3:0]=reg
//===----------------------------------------------------------------------===//

// In-place unary with 2-address constraint: rd = f(rd)
class R_inplace<bits<28> op, string asmstr, list<dag> pat = []>
    : KCInst32<(outs GPR:$rd), (ins GPR:$rs1), asmstr, pat> {
  bits<4> rd;
  let Inst{31-4}  = op;
  let Inst{3-0}   = rd;
  let Constraints = "$rd = $rs1";
  let Defs        = [FLAGS];   // inc/dec/neg/shift-by-1/sext set flags (over-marked; safe)
}

// Input-only R: PUSH, SETSP, JMPR, CALLR
class R_in<bits<28> op, dag ins, string asmstr, list<dag> pat = []>
    : KCInst32<(outs), ins, asmstr, pat> {
  bits<4> reg;
  let Inst{31-4} = op;
  let Inst{3-0}  = reg;
}

// Output-only R: POP, GETSP
class R_out<bits<28> op, dag outs, string asmstr, list<dag> pat = []>
    : KCInst32<outs, (ins), asmstr, pat> {
  bits<4> reg;
  let Inst{31-4} = op;
  let Inst{3-0}  = reg;
}

//===----------------------------------------------------------------------===//
// Format: I0   no operands, fixed opcode
//===----------------------------------------------------------------------===//

class I0<bits<32> op, dag outs, dag ins, string asmstr, list<dag> pat = []>
    : KCInst32<outs, ins, asmstr, pat> {
  let Inst{31-0} = op;
}

//===----------------------------------------------------------------------===//
// Format: RV   register + 32-bit immediate
// [63:36]=op28  [35:32]=rd  [31:0]=imm32
//===----------------------------------------------------------------------===//

// Load immediate into register: rd = imm32
class RV_ld<bits<28> op, string asmstr, list<dag> pat = []>
    : KCInst64<(outs GPR:$rd), (ins simm32:$imm), asmstr, pat> {
  bits<4>  rd;
  bits<32> imm;
  let Inst{63-36} = op;
  let Inst{35-32} = rd;
  let Inst{31-0}  = imm;
}

// In-place: rd op= imm32   ($rd tied to $rs)
class RV_2addr<bits<28> op, string asmstr, list<dag> pat = []>
    : KCInst64<(outs GPR:$rd), (ins GPR:$rs, simm32:$imm), asmstr, pat> {
  bits<4>  rd;
  bits<32> imm;
  let Inst{63-36} = op;
  let Inst{35-32} = rd;
  let Inst{31-0}  = imm;
  let Constraints = "$rd = $rs";
  let Defs        = [FLAGS];   // addv/minusv set flags (bitwise-imm over-marked; safe)
}

// Compare register to immediate — flags only, no output (CMPRV)
class RV_cmp<bits<28> op, string asmstr>
    : KCInst64<(outs), (ins GPR:$rs, simm32:$imm), asmstr> {
  bits<4>  rs;
  bits<32> imm;
  let Inst{63-36} = op;
  let Inst{35-32} = rs;
  let Inst{31-0}  = imm;
  let Defs        = [FLAGS];   // CMPRV sets flags
}

//===----------------------------------------------------------------------===//
// Format: RRV   2 regs + 32-bit offset
// [63:40]=op24  [39:36]=rd  [35:32]=rs  [31:0]=offset32
//===----------------------------------------------------------------------===//

// ALU variant: rd = rs op sign_extend(imm32) — no memory side-effect.
class RRV_alu<bits<24> op, string asmstr, list<dag> pat = []>
    : KCInst64<(outs GPR:$rd), (ins GPR:$rs, simm32:$imm), asmstr, pat> {
  bits<4>  rd;
  bits<4>  rs;
  bits<32> imm;
  let Inst{63-40} = op;
  let Inst{39-36} = rd;
  let Inst{35-32} = rs;
  let Inst{31-0}  = imm;
  let Defs        = [FLAGS];   // addi sets flags
}

// Indexed 64-bit load: rd = mem64[base + offset]
class RRV_ld<bits<24> op, string asmstr, list<dag> pat = []>
    : KCInst64<(outs GPR:$rd), (ins GPR:$base, simm32:$offset), asmstr, pat> {
  bits<4>  rd;
  bits<4>  base;
  bits<32> offset;
  let Inst{63-40} = op;
  let Inst{39-36} = rd;
  let Inst{35-32} = base;
  let Inst{31-0}  = offset;
  let mayLoad     = 1;
  let SchedRW     = [WriteLoad];
}

// Indexed 64-bit store: mem64[base + offset] = rs_data
class RRV_st<bits<24> op, string asmstr, list<dag> pat = []>
    : KCInst64<(outs), (ins GPR:$rs_data, GPR:$base, simm32:$offset), asmstr, pat> {
  bits<4>  rs_data;
  bits<4>  base;
  bits<32> offset;
  let Inst{63-40} = op;
  let Inst{39-36} = rs_data;
  let Inst{35-32} = base;
  let Inst{31-0}  = offset;
  let mayStore    = 1;
  let SchedRW     = [WriteStore];
}

//===----------------------------------------------------------------------===//
// v2 single-word (4-byte) forms that were 2 words in v1.
// Encoding is produced entirely by KlaussCPUMCCodeEmitter.cpp; `Inst` is left
// unset here (documentary only, no generated emitter/decoder consumes it).
//===----------------------------------------------------------------------===//

// Class 4 shift/rotate/bit by embedded constant N (word0[20:15]); 2-address
// ($rd = $rs1).  The count/position operand is a plain immediate; the encoder
// masks it to 6 bits.  (v1: these were 8-byte RV_2addr forms.)
class ShiftImmN<string asmstr, list<dag> pat = []>
    : KCInst32<(outs GPR:$rd), (ins GPR:$rs, simm32:$imm), asmstr, pat> {
  let Constraints = "$rd = $rs";
  let Defs        = [FLAGS];   // shlv/shrv/rolv/rorv set flags
}

// Class 4 bit-test by embedded constant N — flag-only, no destination register
// (Z = ~bit).  (v1: 8-byte RV_cmp form.)
class BitTestImmN<string asmstr>
    : KCInst32<(outs), (ins GPR:$rs, simm32:$imm), asmstr>;

// Class 6/7 register-offset indexed 64-bit load/store (v2: 1 word; the offset
// register number rides in rs2[3:0]).  Modelled with the offset as an
// immediate register number, matching the v1 assembler-only defs.
class Idx64RegOff_ld<string asmstr>
    : KCInst32<(outs GPR:$rd), (ins GPR:$base, simm32:$offset), asmstr> {
  let mayLoad = 1;
  let SchedRW = [WriteLoad];
}
class Idx64RegOff_st<string asmstr>
    : KCInst32<(outs), (ins GPR:$rs_data, GPR:$base, simm32:$offset), asmstr> {
  let mayStore = 1;
  let SchedRW  = [WriteStore];
}

//===----------------------------------------------------------------------===//
// Format: Vimm   fixed opcode + 32-bit target or immediate
// [63:32]=exact_opcode32  [31:0]=value32
//===----------------------------------------------------------------------===//

// Unconditional branch
class Vbr_uncond<bits<32> op, string asmstr, list<dag> pat = []>
    : KCInst64<(outs), (ins BrTarget32:$target), asmstr, pat> {
  bits<32> target;
  let Inst{63-32}            = op;
  let Inst{31-0}             = target;
  let isBranch     = 1;
  let isTerminator = 1;
  let isBarrier    = 1;
  let SchedRW      = [WriteBranch];
}

// Conditional branch (flags set by preceding CMPRR/CMPRV)
class Vbr_cond<bits<32> op, string asmstr>
    : KCInst64<(outs), (ins BrTarget32:$target), asmstr> {
  bits<32> target;
  let Inst{63-32} = op;
  let Inst{31-0}  = target;
  let isBranch    = 1;
  let isTerminator = 1;
  let SchedRW     = [WriteBranch];
  let Uses        = [FLAGS];   // conditional branch reads the flags a compare set
}

// Direct call
class Vcall<bits<32> op, string asmstr>
    : KCInst64<(outs), (ins CallTarget32:$target), asmstr> {
  bits<32> target;
  let Inst{63-32} = op;
  let Inst{31-0}  = target;
  let isCall      = 1;
  let SchedRW     = [WriteBranch];
}

// SP adjustment (ADDSP)
class Vsp<bits<32> op, string asmstr>
    : KCInst64<(outs), (ins simm32:$imm), asmstr> {
  bits<32> imm;
  let Inst{63-32} = op;
  let Inst{31-0}  = imm;
  let SchedRW     = [WriteSP];
}

//===----------------------------------------------------------------------===//
// Format: V64   register ← 64-bit immediate  (3-word, 12-byte)
// Word0[31:8]=op24  Word0[7:4]=rd  Word0[3:0]=0  Word1=lo32  Word2=hi32
// Result: rd = (hi32 << 32) | lo32
//===----------------------------------------------------------------------===//

class V64_ld<bits<24> op, string asmstr, list<dag> pat = []>
    : KCInst96<(outs GPR:$rd), (ins simm32:$lo, simm32:$hi), asmstr, pat> {
  bits<4>  rd;
  bits<32> lo;
  bits<32> hi;
  let Inst{95-72} = op;
  let Inst{71-68} = rd;
  let Inst{67-64} = 0;
  let Inst{63-32} = lo;
  let Inst{31-0}  = hi;
}

//===----------------------------------------------------------------------===//
// ALU — 3-register (RRR)
//===----------------------------------------------------------------------===//

// Arithmetic
def ADDR   : RRR<0x00010, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "addr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (add GPR:$rs1, GPR:$rs2))]>;
def SUBR   : RRR<0x00020, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "subr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (sub GPR:$rs1, GPR:$rs2))]>;

// Bitwise
def ANDR   : RRR<0x00030, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "andr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (and GPR:$rs1, GPR:$rs2))]>;
def ORR    : RRR<0x00040, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "orr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (or  GPR:$rs1, GPR:$rs2))]>;
def XORR   : RRR<0x00050, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "xorr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (xor GPR:$rs1, GPR:$rs2))]>;

// Multiply (EX-serialized, ~5c — M8)
let SchedRW = [WriteMul] in {
def MULR   : RRR<0x00100, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "mulr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (mul GPR:$rs1, GPR:$rs2))]>;
def MULUR  : RRR<0x00110, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "mulur\t$rd, $rs1, $rs2">;          // unsigned lo == signed lo; no extra pattern
def MULHR  : RRR<0x00120, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "mulhr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (mulhs GPR:$rs1, GPR:$rs2))]>;
def MULHUR : RRR<0x00130, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "mulhur\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (mulhu GPR:$rs1, GPR:$rs2))]>;
}

// Divide / modulo (EX-serialized, iterative — M8)
let SchedRW = [WriteDiv] in {
def DIVR   : RRR<0x00140, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "divr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (sdiv GPR:$rs1, GPR:$rs2))]>;
def DIVUR  : RRR<0x00150, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "divur\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (udiv GPR:$rs1, GPR:$rs2))]>;
def MODR   : RRR<0x00160, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "modr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (srem GPR:$rs1, GPR:$rs2))]>;
def MODUR  : RRR<0x00170, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "modur\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (urem GPR:$rs1, GPR:$rs2))]>;
}

// Shifts
def SHLR   : RRR<0x00200, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "shlr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (shl GPR:$rs1, GPR:$rs2))]>;
def SHRR   : RRR<0x00210, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "shrr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (srl GPR:$rs1, GPR:$rs2))]>;
def SARR   : RRR<0x00220, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "sarr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (sra GPR:$rs1, GPR:$rs2))]>;

// Rotate — selected from ISD::ROTL/ROTR (i64 Legal in ISelLowering).
// Rust's u64::rotate_left/right and hash/crypto kernels hit these hard.
def ROLR   : RRR<0x00230, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "rolr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (rotl GPR:$rs1, GPR:$rs2))]>;
def RORR_R : RRR<0x00240, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "rorr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (rotr GPR:$rs1, GPR:$rs2))]>;

// Compare-and-materialise: rd = (rs1 cond rs2) ? 1 : 0
def CMPEQR  : RRR<0x00300, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpeqr\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETEQ)))]>;
def CMPNER  : RRR<0x00310, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpner\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETNE)))]>;
def CMPLTR  : RRR<0x00320, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpltr\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETLT)))]>;
def CMPLER  : RRR<0x00330, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpler\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETLE)))]>;
def CMPGTR  : RRR<0x00340, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpgtr\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETGT)))]>;
def CMPGER  : RRR<0x00350, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpger\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETGE)))]>;
def CMPULTR : RRR<0x00360, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpultr\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETULT)))]>;
def CMPULER : RRR<0x00370, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpuler\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETULE)))]>;
def CMPUGTR : RRR<0x00380, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpugtr\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETUGT)))]>;
def CMPUGER : RRR<0x00390, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 "cmpuger\t$rd, $rs1, $rs2",
                 [(set GPR:$rd, (i64 (setcc GPR:$rs1, GPR:$rs2, SETUGE)))]>;

// Min / Max
def MINR   : RRR<0x00400, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "minr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (smin GPR:$rs1, GPR:$rs2))]>;
def MAXR   : RRR<0x00410, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "maxr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (smax GPR:$rs1, GPR:$rs2))]>;
def MINUR  : RRR<0x00420, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "minur\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (umin GPR:$rs1, GPR:$rs2))]>;
def MAXUR  : RRR<0x00430, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "maxur\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (umax GPR:$rs1, GPR:$rs2))]>;

//===----------------------------------------------------------------------===//
// ALU — 2-register (RR)
//===----------------------------------------------------------------------===//

// Register copy: rd = rs
def COPY_R   : RR1<0x000001, (outs GPR:$rd), (ins GPR:$rs),
                  "copy\t$rd, $rs"> { let SchedRW = [WriteALU]; } // move, not a load

// Compare (flags only): rs1 - rs2, no writeback.
// hasSideEffects = 1 prevents the optimizer from removing or reordering these
// instructions away from the conditional branch that consumes the flags.
def CMPRR_I  : RR0<0x000005, "cmprr\t$rs1, $rs2"> { let hasSideEffects = 1; }

// 64-bit load / store via absolute register address
def MEMGET64 : RR1<0x00007B, (outs GPR:$rd), (ins GPR:$rs),
                  "memget64\t$rd, $rs"> {
  let mayLoad = 1;
}
def MEMSET64 : RRst<0x00007A, "memset64\t$rs_data, $rs_addr">;

// Sub-word register-addressed loads (zero-extend to 64 bits, pure GPR address).
// Indexed variants (base+offset) are LDIDX8/16/32 below.
def MEMGET8  : RR1<0x000075, (outs GPR:$rd), (ins GPR:$rs),
                  "memget8\t$rd, $rs"> { let mayLoad = 1; }
def MEMGET16 : RR1<0x000077, (outs GPR:$rd), (ins GPR:$rs),
                  "memget16\t$rd, $rs"> { let mayLoad = 1; }
def MEMGET32 : RR1<0x000079, (outs GPR:$rd), (ins GPR:$rs),
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
def MEMSET8  : RRst<0x000074, "memset8\t$rs_data, $rs_addr">;
def MEMSET16 : RRst<0x000076, "memset16\t$rs_data, $rs_addr">;
def MEMSET32 : RRst<0x000078, "memset32\t$rs_data, $rs_addr">;

//===----------------------------------------------------------------------===//
// ALU — single-register in-place (2-address: $rd = $rs1)
//===----------------------------------------------------------------------===//

def INCR    : R_inplace<0x0000084, "incr\t$rd">;
def DECR    : R_inplace<0x0000085, "decr\t$rd">;
def NEGR    : R_inplace<0x000008A, "negr\t$rd",
                [(set GPR:$rd, (ineg GPR:$rs1))]>;
def ABSR    : R_inplace<0x000008B, "absr\t$rd",
                [(set GPR:$rd, (abs GPR:$rs1))]>;
def SHLR1   : R_inplace<0x000008D, "shlr1\t$rd">;
def SHRR1   : R_inplace<0x000008E, "shrr1\t$rd">;
def NOTR    : R_inplace<0x0000098, "notr\t$rd",
                [(set GPR:$rd, (not GPR:$rs1))]>;
def POPCNT  : R_inplace<0x00000A8, "popcnt\t$rd",
                [(set GPR:$rd, (ctpop GPR:$rs1))]>;
def CLZ     : R_inplace<0x00000A9, "clz\t$rd",
                [(set GPR:$rd, (ctlz GPR:$rs1))]>;
def CTZ     : R_inplace<0x00000AA, "ctz\t$rd",
                [(set GPR:$rd, (cttz GPR:$rs1))]>;
def BITREV  : R_inplace<0x00000AB, "bitrev\t$rd",
                [(set GPR:$rd, (bitreverse GPR:$rs1))]>;
def BSWAP_R : R_inplace<0x0000097, "bswap\t$rd",
                [(set GPR:$rd, (bswap GPR:$rs1))]>;

// Sign-extend to 64 bits (arch doc §7.2; hardware extends to 64)
def SEXTB   : R_inplace<0x000008C, "sextb\t$rd",
                [(set GPR:$rd, (sext_inreg GPR:$rs1, i8))]>;
def SEXTH   : R_inplace<0x0000094, "sexth\t$rd",
                [(set GPR:$rd, (sext_inreg GPR:$rs1, i16))]>;

// Zero-extend to 64 bits
def ZEXTB   : R_inplace<0x0000095, "zextb\t$rd">;
def ZEXTH   : R_inplace<0x0000096, "zexth\t$rd">;

// Sign/zero-extend 32→64 bits (hardware fixed April 2026)
def SEXTW   : R_inplace<0x00000F0, "sextw\t$rd",
                [(set GPR:$rd, (sext_inreg GPR:$rs1, i32))]>;
def ZEXTW   : R_inplace<0x00000F1, "zextw\t$rd">;

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
def ADJCALLSTACKDOWN : Pseudo<(outs), (ins i32imm:$amt1, i32imm:$amt2)>;
def ADJCALLSTACKUP   : Pseudo<(outs), (ins i32imm:$amt1, i32imm:$amt2)>;

// SELECT without CMOV: expanded to compare + conditional branches by
// EmitInstrWithCustomInserter in KlaussCPUInstrInfo.cpp.
let usesCustomInserter = 1, hasSideEffects = 0 in
def SELECT_PSEUDO : Pseudo<(outs GPR:$dst),
                            (ins GPR:$cond, GPR:$trueV, GPR:$falseV)>;

//===----------------------------------------------------------------------===//
// Stack management (used by frame lowering; no isel patterns)
//===----------------------------------------------------------------------===//

let Uses = [SP], Defs = [SP], mayStore = 1 in
def PUSH_R  : R_in<0x0000400, (ins GPR:$reg), "push\t$reg">;
let Uses = [SP], Defs = [SP], mayLoad = 1 in
def POP_R   : R_out<0x0000401, (outs GPR:$reg), "pop\t$reg">;
let Uses = [SP], hasSideEffects = 1 in
def GETSP_R : R_out<0x0000403, (outs GPR:$reg), "getsp\t$reg">;
let Defs = [SP], hasSideEffects = 1 in
def SETSP_R : R_in<0x0000404, (ins GPR:$reg), "setsp\t$reg">;
let Uses = [SP], Defs = [SP], hasSideEffects = 1 in
def ADDSP_I : Vsp<0x00004050, "addsp\t$imm">;

// Indirect call / jump via register
// Defs: all caller-saved GPRs clobbered by the ABI (R0-R3=args, R8-R14=temps+return)
let isCall = 1,
    Uses = [SP],
    Defs = [R0, R1, R2, R3, R8, R9, R10, R11, R12, R13, R14] in
def CALLR_R : R_in<0x0000407, (ins GPR:$reg), "callr\t$reg">;
def JMPR_R  : R_in<0x0000102, (ins GPR:$reg), "jmpr\t$reg"> {
  let isBranch         = 1;
  let isIndirectBranch = 1;
  let isTerminator     = 1;
  let isBarrier        = 1;  // indirect branch never falls through
}

//===----------------------------------------------------------------------===//
// RV: register ← immediate  (SETR and in-place RV ops)
//===----------------------------------------------------------------------===//

// Load 32-bit immediate (sign-extended to 64 bits)
def SETR    : RV_ld<0x0000080, "setr\t$rd, $imm">;

// Load full 64-bit immediate: rd = (hi32 << 32) | lo32
def SETR64  : V64_ld<0x0000FE, "setr64\t$rd, $lo, $hi">;

// In-place immediate operations (rd op= imm32)
def ADDV    : RV_2addr<0x0000081, "addv\t$rd, $imm">;
def MINUSV  : RV_2addr<0x0000082, "minusv\t$rd, $imm">;
def ANDV    : RV_2addr<0x0000086, "andv\t$rd, $imm">;
def ORV     : RV_2addr<0x0000087, "orv\t$rd, $imm">;
def XORV    : RV_2addr<0x0000088, "xorv\t$rd, $imm">;
// Shift by immediate (count = imm[5:0]); v2: single word, count embedded.
def SHLV    : ShiftImmN<"shlv\t$rd, $imm">;
def SHRV    : ShiftImmN<"shrv\t$rd, $imm">;
def SHRAV   : ShiftImmN<"shrav\t$rd, $imm">;
def MULV    : RV_2addr<0x00000B8, "mulv\t$rd, $imm">;
def DIVV    : RV_2addr<0x00000B9, "divv\t$rd, $imm">;
def MODV    : RV_2addr<0x00000BA, "modv\t$rd, $imm">;

// Rotate by immediate (count = imm[5:0]); v2: single word.  Register-amount
// forms are ROLR/RORR_R.
def ROLV    : ShiftImmN<"rolv\t$rd, $imm">;
def RORV    : ShiftImmN<"rorv\t$rd, $imm">;

// reg+imm32 add: rd = rs + sign_extend(imm32). Two distinct regs; sign-extends.
// Replaces the SETR+ADDR two-instruction sequence for frame-address materialisation.
// Pattern deferred: simm32_imm is not yet defined here; see isel patterns section.
def ADDI : RRV_alu<0x000002, "addi\t$rd, $rs, $imm">;

// Compare register to immediate — flags only (same side-effect caveat as CMPRR_I)
def CMPRV_I : RV_cmp<0x0000083, "cmprv\t$rs, $imm"> { let hasSideEffects = 1; }

//===----------------------------------------------------------------------===//
// Memory: indexed load / store  (primary frame-slot instructions)
//
// Opcodes from opcode_select.vh (authoritative hardware source — the
// fpga/KlaussCPU/.../opcode_select.vh casez, NOT the stale klatest copies):
//   LDIDX64  = 0x0000FC?? (64-bit indexed load)
//   STIDX64  = 0x0000FD?? (64-bit indexed store)
//
// IMPORTANT: opcodes 0xFC/0xFD are the *force-aligned* hardware variants
// (LDIDX64A/STIDX64A) — the RTL computes (rs2 + imm) & ~7 before the bus
// access.  The non-aligned 64-bit indexed ops live at 0x0C/0x0D.  Using the
// force-aligned form is only safe because LLVM never emits a misaligned i64
// access: allowsMisalignedMemoryAccesses() is left at its default (false), so
// under-aligned i64 loads/stores are expanded to byte ops by the legalizer.
// If misaligned i64 access is ever enabled, switch these to 0x0C/0x0D first
// (and confirm the HW handles a 64-bit read spanning two cache lines).
//   LDIDX32  = 0x0000C0?? (32-bit indexed load, zero-extends to 64)
//   STIDX32  = 0x0000C1?? (32-bit indexed store)
//   LDIDX16  = 0x0000C2?? (16-bit indexed load, zero-extends to 64)
//   STIDX16  = 0x0000C3?? (16-bit indexed store)
//   LDIDX8   = 0x0000C4?? (8-bit indexed load,  zero-extends to 64)
//   STIDX8   = 0x0000C5?? (8-bit indexed store)
// All are RRV format: [63:40]=op24 [39:36]=rd/rs_data [35:32]=base [31:0]=offset32
//===----------------------------------------------------------------------===//

def LDIDX64 : RRV_ld<0x0000FC, "ldidx64\t$rd, $base, $offset">;
def STIDX64 : RRV_st<0x0000FD, "stidx64\t$rs_data, $base, $offset">;

def LDIDX32 : RRV_ld<0x0000C0, "ldidx32\t$rd, $base, $offset">;
// STIDX32/STIDX16 mask addr&~3 / addr&~1 like MEMSET32/16 — safe only while
// allowsMisalignedMemoryAccesses()==false. See the alignment warning above the
// MEMSET8/16/32 defs before enabling any unaligned store path.
def STIDX32 : RRV_st<0x0000C1, "stidx32\t$rs_data, $base, $offset">;

def LDIDX16   : RRV_ld<0x0000C2, "ldidx16\t$rd, $base, $offset">   { let mayLoad = 1; }
def STIDX16   : RRV_st<0x0000C3, "stidx16\t$rs_data, $base, $offset">;
def LDIDX8    : RRV_ld<0x0000C4, "ldidx8\t$rd, $base, $offset">    { let mayLoad = 1; }
def STIDX8    : RRV_st<0x0000C5, "stidx8\t$rs_data, $base, $offset">;
def LDIDX8_S  : RRV_ld<0x0000C6, "ldidx8s\t$rd, $base, $offset">   { let mayLoad = 1; }
def LDIDX16_S : RRV_ld<0x0000C7, "ldidx16s\t$rd, $base, $offset">  { let mayLoad = 1; }

// Accept the canonical opcode_select.vh spelling of the signed sub-word loads
// (LDIDX8_S / LDIDX16_S) as assembler aliases for the printed mnemonics.
// Mnemonics reach the matcher lower-cased, so the alias keys are lower-case.
def : MnemonicAlias<"ldidx8_s",  "ldidx8s">;
def : MnemonicAlias<"ldidx16_s", "ldidx16s">;

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
// These fold a constant operand directly into the RV_2addr / R_inplace
// encoding, avoiding the separate SETR (8 B) that the register-register form
// would need to materialise the constant.  This matters disproportionately on
// KlaussCPU because the core is fetch-bound — fewer fetched words ≈ fewer
// cycles.  The encoder (KlaussCPUMCCodeEmitter) already handles all of these
// opcodes, so this is a pure pattern-matching addition.
//
// Extension semantics come straight from opcode_select.vh and gate each
// pattern — getting these wrong would silently miscompile:
//   SHLV/SHRV/SHRAV : shift count is imm[5:0]            → uimm6   (0–63)
//   ANDV/ORV/XORV   : imm32 is ZERO-extended to 64 bits  → uimm32_imm (hi32==0)
//   MULV            : imm32 is SIGN-extended to 64 bits   → simm32_imm
// Out-of-range constants simply fall through to the register-register form.
//===----------------------------------------------------------------------===//

def uimm6      : ImmLeaf<i64, [{ return isUInt<6>(Imm); }]>;
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

def JMP     : Vbr_uncond<0x00001000, "jmp\t$target">;

def JMPZ    : Vbr_cond<0x00001001, "jmpz\t$target">;
def JMPNZ   : Vbr_cond<0x00001002, "jmpnz\t$target">;
def JMPE    : Vbr_cond<0x00001003, "jmpe\t$target">;
def JMPNE   : Vbr_cond<0x00001004, "jmpne\t$target">;
def JMPC    : Vbr_cond<0x00001005, "jmpc\t$target">;
def JMPNC   : Vbr_cond<0x00001006, "jmpnc\t$target">;
def JMPS    : Vbr_cond<0x00001013, "jmps\t$target">;
def JMPNS   : Vbr_cond<0x00001014, "jmpns\t$target">;
def JMPLT   : Vbr_cond<0x00001015, "jmplt\t$target">;
def JMPLE   : Vbr_cond<0x00001016, "jmple\t$target">;
def JMPGT   : Vbr_cond<0x00001017, "jmpgt\t$target">;
def JMPGE   : Vbr_cond<0x00001018, "jmpge\t$target">;
def JMPULT  : Vbr_cond<0x00001019, "jmpult\t$target">;
def JMPULE  : Vbr_cond<0x0000101A, "jmpule\t$target">;
def JMPUGT  : Vbr_cond<0x0000101B, "jmpugt\t$target">;
def JMPUGE  : Vbr_cond<0x0000101C, "jmpuge\t$target">;

// Defs: all caller-saved GPRs clobbered by the ABI (R0-R3=args, R8-R14=temps+return)
let Uses = [SP],
    Defs = [R0, R1, R2, R3, R8, R9, R10, R11, R12, R13, R14] in
def CALL_I  : Vcall<0x00001009, "call\t$target">;

//===----------------------------------------------------------------------===//
// PC-relative flow control (opcodes 0x1030–0x1041, 0x099)
// target = PC_of_this_instruction + sign_ext(imm32)   (option-B convention)
// Used in PIC mode (-fPIC / -relocation-model=pic).
// The encoder emits FK_KlaussCPU_PCREL32 so the assembler/linker fills in
// (symbol_addr − instr_addr).
//===----------------------------------------------------------------------===//

// PC-relative unconditional branch
def JMPREL   : Vbr_uncond<0x00001030, "jmprel\t$target">;

// PC-relative conditional branches — flag sources same as absolute counterparts
def JMPZREL   : Vbr_cond<0x00001031, "jmpzrel\t$target">;   // zero_flag
def JMPNZREL  : Vbr_cond<0x00001032, "jmpnzrel\t$target">;  // !zero_flag
def JMPEREL   : Vbr_cond<0x00001033, "jmperel\t$target">;   // equal_flag
def JMPNEREL  : Vbr_cond<0x00001034, "jmpnerel\t$target">;  // !equal_flag
def JMPCREL   : Vbr_cond<0x00001035, "jmpcrel\t$target">;   // carry_flag
def JMPNCREL  : Vbr_cond<0x00001036, "jmpncrel\t$target">;  // !carry_flag
def JMPSREL   : Vbr_cond<0x00001037, "jmpsrel\t$target">;   // sign_flag
def JMPNSREL  : Vbr_cond<0x00001038, "jmpnsrel\t$target">;  // !sign_flag
def JMPLTREL  : Vbr_cond<0x00001039, "jmpltrel\t$target">;  // less_flag
def JMPLEREL  : Vbr_cond<0x0000103A, "jmplerel\t$target">;  // less|equal
def JMPGTREL  : Vbr_cond<0x0000103B, "jmpgtrel\t$target">;  // !less&!equal
def JMPGEREL  : Vbr_cond<0x0000103C, "jmpgerel\t$target">;  // !less_flag
def JMPULTREL : Vbr_cond<0x0000103D, "jmpultrel\t$target">; // ult_flag
def JMPULEREL : Vbr_cond<0x0000103E, "jmpulerel\t$target">; // ult|equal
def JMPUGTREL : Vbr_cond<0x0000103F, "jmpugtrel\t$target">; // !ult&!equal
def JMPUGEREL : Vbr_cond<0x00001040, "jmpugerel\t$target">; // !ult_flag

// PC-relative direct call: push PC+8 (return addr), PC += sign_ext(imm32)
let Uses = [SP],
    Defs = [R0, R1, R2, R3, R8, R9, R10, R11, R12, R13, R14] in
def CALLREL : Vcall<0x00001041, "callrel\t$target">;

// LEAPC: rd = PC_of_instruction + sign_ext(imm32)  (address materialisation)
// Opcode 0x0000099 in RV_ld slot: word0 = (0x099 << 4) | rd, word1 = simm32.
// The encoder emits FK_KlaussCPU_PCREL32 for symbol references.
def LEAPC   : RV_ld<0x0000099, "leapc\t$rd, $imm">;

//===----------------------------------------------------------------------===//
// Special / system
//===----------------------------------------------------------------------===//

let isReturn = 1, isTerminator = 1, isBarrier = 1 in
def RET_I   : I0<0x00001012, (outs), (ins), "ret", [(KlaussCPURetGlue)]>;
def NOP_I   : I0<0x0000F010, (outs), (ins), "nop">;
def HALT_I  : I0<0x0000F011, (outs), (ins), "halt">;
def IRET_I  : I0<0x00006011, (outs), (ins), "iret">;

// WAIT — interruptible core-suspend: stop fetching/executing and resume when an
// unmasked, vectored interrupt becomes pending (timer tick, future wired source).
// No register/flag side effects; IRET resumes at PC+4.  Distinct from HALT (which
// stops permanently).  hasSideEffects keeps it from being reordered/eliminated.
let hasSideEffects = 1 in
def WAIT_I  : I0<0x00006012, (outs), (ins), "wait">;

//===----------------------------------------------------------------------===//
// Peripheral I-O instructions (hasSideEffects prevents reordering/elimination)
//
// UART is memory-mapped (MMIO at 0xF001_0000) and accessed via ordinary
// volatile loads/stores — there are no UART opcodes/intrinsics anymore.
//===----------------------------------------------------------------------===//

let hasSideEffects = 1 in {
  // ── Legacy peripheral opcodes (still in hardware; accessible via inline asm)
  // Set 16 on-board LEDs from register bits [15:0].
  def LEDR_R      : R_in<0x0000300, (ins GPR:$reg), "ledr\t$reg">;
  // Set 7-seg display 1 from register.
  def SEG7_1R_R   : R_in<0x0000302, (ins GPR:$reg), "7seg1r\t$reg">;
  // Set 7-seg display 2 from register.
  def SEG7_2R_R   : R_in<0x0000303, (ins GPR:$reg), "7seg2r\t$reg">;
  // Set both 7-seg displays from register.
  def SEG7R_R     : R_in<0x0000304, (ins GPR:$reg), "7segr\t$reg">;
  // Blank both 7-seg displays.
  def SEG7BLANK_I : I0<0x00003073, (outs), (ins), "7segblank">;
}

//===----------------------------------------------------------------------===//
// Assembler-only opcodes — full opcode_select.vh coverage
//
// These instructions are NOT emitted by the code generator (no isel Pattern);
// they exist so hand-written `.s` / inline-asm can name every hardware opcode
// in opcode_select.vh.  Opcode values and formats are taken verbatim from that
// file, so the bytes produced by KlaussCPUMCCodeEmitter match the hardware-
// verified `klausscc` assembler.  Each new def needs a matching `case` in
// KlaussCPUMCCodeEmitter.cpp.
//===----------------------------------------------------------------------===//

// ── RRR (rd, rs1, rs2) ─────────────────────────────────────────────────────
def ADDC   : RRR<0x00060, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "addc\t$rd, $rs1, $rs2"> { let Uses = [FLAGS]; } // reads carry-in
def SUBC   : RRR<0x00070, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "subc\t$rd, $rs1, $rs2"> { let Uses = [FLAGS]; } // reads borrow-in
def BSETRR : RRR<0x00500, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "bsetrr\t$rd, $rs1, $rs2">;
def BCLRRR : RRR<0x00510, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "bclrrr\t$rd, $rs1, $rs2">;
def BTGLRR : RRR<0x00520, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "btglrr\t$rd, $rs1, $rs2">;
def BTSTRR : RRR<0x00530, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "btstrr\t$rd, $rs1, $rs2">;

// ── R in-place (rd = f(rd); 2-address) ─────────────────────────────────────
def SETFR : R_inplace<0x0000089, "setfr\t$rd">;
def SHLAR : R_inplace<0x000008F, "shlar\t$rd">;
def SHRAR : R_inplace<0x0000090, "shrar\t$rd">;
def ROLR1 : R_inplace<0x00000F8, "rolr1\t$rd">;
def RORR1 : R_inplace<0x00000F9, "rorr1\t$rd">;
def ROLCR : R_inplace<0x00000FA, "rolcr\t$rd"> { let Uses = [FLAGS]; } // rotate through carry
def RORCR : R_inplace<0x00000FB, "rorcr\t$rd"> { let Uses = [FLAGS]; } // rotate through carry

// ── RV 2-address (rd op= imm32) and RV compare (rs, imm32) ──────────────────
// Bit position / start+len are packed in the immediate (see opcode_select.vh).
// Bit set/clear/toggle by embedded position N — v2: single word, 2-address.
def BSET  : ShiftImmN<"bset\t$rd, $imm">;
def BCLR  : ShiftImmN<"bclr\t$rd, $imm">;
def BTGL  : ShiftImmN<"btgl\t$rd, $imm">;
// Bit-field extract/deposit stay 2 words (params packed in imm32).
def BEXTR : RV_2addr<0x00000AC, "bextr\t$rd, $imm">;
def BDEP  : RV_2addr<0x00000AD, "bdep\t$rd, $imm">;
// Bit-test by embedded position N — v2: single word, flag-only.
def BTST  : BitTestImmN<"btst\t$rs, $imm">;

// ── RR (rd, rs) load / (rs_data, rs_addr) store — 8-byte aligned ────────────
def MEMREADRR  : RR1<0x000071, (outs GPR:$rd), (ins GPR:$rs), "memreadrr\t$rd, $rs"> { let mayLoad = 1; }
def MEMSET64RR : RRst<0x000070, "memset64rr\t$rs_data, $rs_addr">;

// ── RV absolute memory access (rd/rs, address-imm32) — symbol-capable ───────
def MEMREADR : RV_ld <0x0000721, "memreadr\t$rd, $imm"> { let mayLoad = 1; }
def MEMSETR  : RV_cmp<0x0000720, "memsetr\t$rs, $imm"> { let mayStore = 1; }

// ── RRV 64-bit indexed: non-aligned (vh LDIDX64/STIDX64 = 0x0C/0x0D) and
//    register-offset (vh LDIDX64R/STIDX64R).  NOTE: LLVM's ldidx64/stidx64 are
//    the *force-aligned* 0xFC/0xFD (= vh LDIDX64A/STIDX64A); aliases below.
//    For the *R variants the offset operand is a register *number* in imm[3:0].
def LDIDX64U : RRV_ld<0x00000C, "ldidx64u\t$rd, $base, $offset">;
def STIDX64U : RRV_st<0x00000D, "stidx64u\t$rs_data, $base, $offset">;
// v2: register-offset indexed ops are a single word (offset reg in rs2[3:0]).
def LDIDX64R : Idx64RegOff_ld<"ldidx64r\t$rd, $base, $offset">;
def STIDX64R : Idx64RegOff_st<"stidx64r\t$rs_data, $base, $offset">;

// ── V (fixed op32 + imm32/address) — push / conditional jumps ───────────────
def PUSHV : Vsp<0x00004020, "pushv\t$imm"> { let mayStore = 1; }
def JMPO  : Vbr_cond<0x00001007, "jmpo\t$target">;
def JMPNO : Vbr_cond<0x00001008, "jmpno\t$target">;

// ── Vcall (conditional calls) ───────────────────────────────────────────────
def CALLZ  : Vcall<0x0000100A, "callz\t$target">;
def CALLNZ : Vcall<0x0000100B, "callnz\t$target">;
def CALLE  : Vcall<0x0000100C, "calle\t$target">;
def CALLNE : Vcall<0x0000100D, "callne\t$target">;
def CALLC  : Vcall<0x0000100E, "callc\t$target">;
def CALLNC : Vcall<0x0000100F, "callnc\t$target">;
def CALLO  : Vcall<0x00001010, "callo\t$target">;
def CALLNO : Vcall<0x00001011, "callno\t$target">;

// ── V64 (push 64-bit immediate; 12-byte) ────────────────────────────────────
class V64_imm<bits<32> op, string asmstr>
    : KCInst96<(outs), (ins simm32:$lo, simm32:$hi), asmstr> {
  bits<32> lo;
  bits<32> hi;
  let Inst{95-64} = op;
  let Inst{63-32} = lo;
  let Inst{31-0}  = hi;
  let SchedRW     = [WriteSP];
}
def PUSHV64 : V64_imm<0x00004060, "pushv64\t$lo, $hi"> { let mayStore = 1; }

// ── Peripheral / timing ops (side-effecting) ────────────────────────────────
let hasSideEffects = 1 in {
  // R_in (single source register)
  def DELAYR   : R_in<0x0000F00, (ins GPR:$reg), "delayr\t$reg">;
  def RGB1R    : R_in<0x0000305, (ins GPR:$reg), "rgb1r\t$reg">;
  def RGB2R    : R_in<0x0000306, (ins GPR:$reg), "rgb2r\t$reg">;
  def LCDDATAR : R_in<0x0000201, (ins GPR:$reg), "lcddatar\t$reg">;
  // V (fixed op32 + imm32)
  def DELAYV   : Vsp<0x0000F013, "delayv\t$imm">;
  def LEDV     : Vsp<0x00003070, "ledv\t$imm">;
  def RGB1V    : Vsp<0x00003074, "rgb1v\t$imm">;
  def RGB2V    : Vsp<0x00003075, "rgb2v\t$imm">;
  def LCD      : Vsp<0x00002023, "lcd\t$imm">;
  def LCDCMDV  : Vsp<0x00002021, "lcdcmdv\t$imm">;
  def LCDDATAV : Vsp<0x00002022, "lcddatav\t$imm">;
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
