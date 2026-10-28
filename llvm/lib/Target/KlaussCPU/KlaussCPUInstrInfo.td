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

// Tail call (LowerCall): a jump to the callee in place of the return.
def KlaussCPUTailNode : SDNode<"KlaussCPUISD::TAIL", SDT_KlaussCPUCall,
                               [SDNPHasChain, SDNPOptInGlue, SDNPVariadic]>;

// Call frame setup/teardown (ADJCALLSTACKDOWN/UP).  LowerCall builds them
// with getCALLSEQ_START/END, whose operands are pointer-sized (i64) target
// constants.
def SDT_KlaussCPUCallSeqStart : SDCallSeqStart<[SDTCisVT<0, i64>,
                                                SDTCisVT<1, i64>]>;
def SDT_KlaussCPUCallSeqEnd   : SDCallSeqEnd<[SDTCisVT<0, i64>,
                                              SDTCisVT<1, i64>]>;
def callseq_start : SDNode<"ISD::CALLSEQ_START", SDT_KlaussCPUCallSeqStart,
                           [SDNPHasChain, SDNPOutGlue]>;
def callseq_end   : SDNode<"ISD::CALLSEQ_END", SDT_KlaussCPUCallSeqEnd,
                           [SDNPHasChain, SDNPOptInGlue, SDNPOutGlue]>;

// Address of a global, external symbol, block address or jump table, from
// LowerGlobalAddress & co: SETRU (ABS32 fixup) or, under PIC, LEAPC (PCREL32).
// The wrapper keeps the target symbol node, which CSEs, from being the node
// that selection replaces.
def KlaussCPUAddr  : SDNode<"KlaussCPUISD::ADDR", SDTIntUnaryOp>;
def KlaussCPULeaPC : SDNode<"KlaussCPUISD::LEAPC", SDTIntUnaryOp>;

// 32-bit rotates of the low half (ReplaceNodeResults for an i32 rotl/rotr).
def KlaussCPURolW : SDNode<"KlaussCPUISD::ROLW", SDTIntShiftOp>;
def KlaussCPURorW : SDNode<"KlaussCPUISD::RORW", SDTIntShiftOp>;

//===----------------------------------------------------------------------===//
// Instruction selection predicates
//===----------------------------------------------------------------------===//

def IsPIC  : Predicate<"TM.isPositionIndependent()">;
def NotPIC : Predicate<"!TM.isPositionIndependent()">;
// -mllvm -klausscpu-w32-alu (KlaussCPUISelDAGToDAG.cpp), on by default.
def UseW32Alu : Predicate<"EnableW32Alu">;


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
// Short branches are AsmVariantName "NoMatch" (the parser emits the 2-word
// form and its CompressPat shortens it), so this class never reaches
// the matcher; it gives PCRelTarget18 a class consistent with the others.
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
// PC-relative operands decode to the byte displacement from the instruction;
// they are OPERAND_PCREL, which is how the InstPrinter and MCInstrAnalysis
// find them.
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
// The MCOperandPredicate is required of an operand a short branch expands
// into (JMPO/JMPNO); as PCRelTarget18 is symbolic, it always takes the isExpr
// path there.
def BrTarget32 : Operand<OtherVT> {
  let EncoderMethod    = "getAbs32OpValue";
  let DecoderMethod    = "decodeUImmOperand<32>";
  let ParserMatchClass = UImm32SymAsmOperand;
  let MCOperandPredicate = [{
    int64_t Imm;
    if (MCOp.evaluateAsConstantImm(Imm))
      return isUInt<32>(Imm);
    return MCOp.isExpr();
  }];
}

def CallTarget32 : Operand<i64> {
  let EncoderMethod    = "getAbs32OpValue";
  let DecoderMethod    = "decodeUImmOperand<32>";
  let ParserMatchClass = UImm32SymAsmOperand;
}

// PC-relative branch/call target and LEAPC offset: a 32-bit byte
// displacement from the instruction in word 1 (FK_KlaussCPU_PCREL32).
// MCOperandPredicate: as BrTarget32 (the JMP*REL a short branch expands into).
def PCRelTarget32 : Operand<OtherVT> {
  let EncoderMethod    = "getPCRel32OpValue";
  let DecoderMethod    = "decodeSImmOperand<32>";
  let ParserMatchClass = PCRel32AsmOperand;
  let OperandType      = "OPERAND_PCREL";
  let MCOperandPredicate = [{
    int64_t Imm;
    if (MCOp.evaluateAsConstantImm(Imm))
      return isInt<32>(Imm);
    return MCOp.isExpr();
  }];
}

def PCRelCallTarget32 : Operand<i64> {
  let EncoderMethod    = "getPCRel32OpValue";
  let DecoderMethod    = "decodeSImmOperand<32>";
  let ParserMatchClass = PCRel32AsmOperand;
  let OperandType      = "OPERAND_PCREL";
  let MCOperandPredicate = [{
    int64_t Imm;
    if (MCOp.evaluateAsConstantImm(Imm))
      return isInt<32>(Imm);
    return MCOp.isExpr();
  }];
}

def pcrel_simm32 : Operand<i64> {
  let EncoderMethod    = "getPCRel32OpValue";
  let DecoderMethod    = "decodeSImmOperand<32>";
  let ParserMatchClass = PCRel32AsmOperand;
  let OperandType      = "OPERAND_PCREL";
}

// ISA v3 short branch target: byte displacement, encoded as a simm18 word
// displacement in word0[17:0] (FK_KlaussCPU_PCREL18).  Only a symbolic
// target compresses into a short branch (the CompressPats below): the asm
// backend relaxes it back when the target is out of range or not local.  A
// literal target stays in the 2-word form it was written in.
def PCRelTarget18 : Operand<OtherVT>,
                    ImmLeaf<i64, [{return isShiftedInt<18, 2>(Imm);}]> {
  let EncoderMethod    = "getPCRel18OpValue";
  let DecoderMethod    = "decodeSImmOperand<20>";
  let ParserMatchClass = PCRel20AsmOperand;
  let OperandType      = "OPERAND_PCREL";
  let MCOperandPredicate = [{ return MCOp.isExpr(); }];
}

// ISA v3 short call target: as PCRelTarget18, but a target the assembler
// cannot resolve becomes an R_KLAUSSCPU_CALL18 relocation
// (FK_KlaussCPU_CALL18) instead of relaxing; lld adds a thunk when the
// callee is out of range.  Only a symbolic target compresses.
def CallTarget18 : Operand<i64>,
                   ImmLeaf<i64, [{return isShiftedInt<18, 2>(Imm);}]> {
  let EncoderMethod    = "getCall18OpValue";
  let DecoderMethod    = "decodeSImmOperand<20>";
  let ParserMatchClass = PCRel20AsmOperand;
  let OperandType      = "OPERAND_PCREL";
  let MCOperandPredicate = [{ return MCOp.isExpr(); }];
}

// ISA v3 fused-branch target: byte displacement, encoded as a simm13 word
// displacement in word0[20:8] (FK_KlaussCPU_PCREL13).
def PCRelTarget13 : Operand<OtherVT> {
  let EncoderMethod    = "getPCRel13OpValue";
  let DecoderMethod    = "decodeSImmOperand<15>";
  let ParserMatchClass = PCRel15AsmOperand;
  let OperandType      = "OPERAND_PCREL";
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
  // from their 2-word twin's.  Its LEN must match Size.
  bits<32> Word0 = word0;
}

class KCInst32<bits<32> word0, dag outs, dag ins, string asmstr,
               list<dag> pat = []>
    : KCInst<4, word0, outs, ins, asmstr, pat> {
  field bits<32> Inst = word0;
  field bits<32> SoftFail = 0;
  assert !eq(word0{31-30}, 0b01), NAME # ": a 1-word template needs LEN=01";
}

// imm32 / target in word 1.
class KCInst64<bits<32> word0, dag outs, dag ins, string asmstr,
               list<dag> pat = []>
    : KCInst<8, word0, outs, ins, asmstr, pat> {
  field bits<64> Inst;
  field bits<64> SoftFail = 0;
  let Inst{31-0} = word0;
  assert !eq(word0{31-30}, 0b10), NAME # ": a 2-word template needs LEN=10";
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
  let Defs       = [FLAGS];   // see "Which instructions write FLAGS"
  let SchedRW    = [WriteALU, WriteFlags];
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

// Register-offset indexed load/store (MODE=11, 1 word): EA = rs1 + rs2 (the
// offset register is a real GPR operand in rs2).
class IdxRegOff_ld<bits<32> word0, string asmstr>
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
class IdxRegOff_st<bits<32> word0, string asmstr>
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
  let Defs        = [FLAGS];   // see "Which instructions write FLAGS"
  let SchedRW     = [WriteALU, WriteFlags];
}

// Unary 3-operand form: rd = f(rs1).  ISA v2 encodes rd and rs1 separately
// ("rd≠rs1 also works, same hardware path" — ISA_ENCODING_V2_MAP.md §4), so
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
  let Defs       = [FLAGS];   // see "Which instructions write FLAGS"
  let SchedRW    = [WriteALU, WriteFlags];
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
  let Defs        = [FLAGS];   // see "Which instructions write FLAGS"
  let SchedRW     = [WriteALU, WriteFlags];
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
  let Defs        = [FLAGS];   // see "Which instructions write FLAGS"
  let SchedRW     = [WriteALU, WriteFlags];
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
class RV_cmp<bits<32> word0, string asmstr, Operand immop = simm32>
    : KCInst64<word0, (outs), (ins GPR:$rs, immop:$imm), asmstr> {
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
  let SchedRW     = [WriteALU, WriteFlags];
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
// Which instructions write FLAGS
//
// The register formats default to Defs = [FLAGS], the safe side: a missing
// def is a miscompile (the schedulers, FlagReuse and FuseCmpBr move or
// delete compares across instructions that do not define FLAGS), a spare
// one only costs freedom.  The instructions whose decode sets no flag write
// mask (pipeline_core.sv f_decode_raw `d.fmask`, committed at WB through
// `wb_fmask`) clear it, and the FLAGS half of their SchedRW, in a
// `let Defs = []<Register>, SchedRW = [WriteALU]` block:
//   - AND/OR/XOR in either form, MIN/MAX(U): class 1/2 ops with [21] = 0
//     (`{4'd4..10, 1'b0}`); the boolean compares (class 3 B = 1, U_BOOL);
//   - the bit set/clear/toggle ops and BTSTRR, and the shift-by-one
//     SHLR1/SHRR1/SHRAR: class 4 with F [14] = 0;
//   - BSWAP/BITREV/CLZ/CTZ/SEXTW/ZEXTW and GETF (SETFR): class 5 with
//     F [19] = 0.
// What the others write: ADD/SUB/ADC/SBC/INC/DEC/CMP and the W forms Z S C
// V; a shift or rotate (F = 1 in every template) Z, a rotate by an embedded
// 1 Z C (U_ROT1); BTST/BEXTR/NEG/NOT/ZEXTB/ZEXTH/POPCNT Z; SEXTB/SEXTH Z S;
// ABS Z V; MUL Z S V; DIV/MOD Z V (V only for a zero divisor); IRET all
// four.  Z is (result == 0) for all of them but BTST (Z = ~bit), and DIV/MOD
// by zero leaves it.  test_flags in the runtime repo (baremetal/programs,
// generated from this file by gen_test_flags.py) checks every instruction
// on the emulator or the board against these defs.
//===----------------------------------------------------------------------===//

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

// Bitwise (no flags)
let Defs = []<Register>, SchedRW = [WriteALU] in {
def ANDR   : RRR<0x45000000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "andr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (and GPR:$rs1, GPR:$rs2))]>;
def ORR    : RRR<0x45400000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "orr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (or  GPR:$rs1, GPR:$rs2))]>;
def XORR   : RRR<0x45800000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "xorr\t$rd, $rs1, $rs2",
                [(set GPR:$rd, (xor GPR:$rs1, GPR:$rs2))]>;
}

// ISA v3 D3: 32-bit "W" ops — the 64-bit op with its result sign-extended
// from bit 31 (RV64-style: C `int` arithmetic stays sign-extended without a
// following SEXTW). Class 1 / class 2 (2-word) / class A with W [19].
// Selected by the W-op patterns (-mllvm -klausscpu-w32-alu=false turns
// them off).
def ADDW   : RRR<0x44280000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "addw\t$rd, $rs1, $rs2">;
def SUBW   : RRR<0x44680000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "subw\t$rd, $rs1, $rs2">;
def ADDIW  : RRV_alu<0x88380000, "addiw\t$rd, $rs, $imm">;
let SchedRW = [WriteMul, WriteMulFlags] in
def MULW   : RRR<0x68880000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "mulw\t$rd, $rs1, $rs2">;

// Multiply (EX-serialized, ~5c — M8)
let SchedRW = [WriteMul, WriteMulFlags] in {
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
let SchedRW = [WriteDiv, WriteDivFlags] in {
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

// Shifts and rotates by a register amount.  Selected by the shamt patterns
// below (the CPU reads only rs2[5:0]).  Rotates come from ISD::ROTL/ROTR
// (i64 Legal in ISelLowering; Rust's u64::rotate_left/right and hash/crypto
// kernels hit these hard) and from the 32-bit rotate nodes.
let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in {
def SHLR   : RRR<0x50004000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "shlr\t$rd, $rs1, $rs2">;
def SHRR   : RRR<0x50404000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "shrr\t$rd, $rs1, $rs2">;
def SARR   : RRR<0x50804000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "sarr\t$rd, $rs1, $rs2">;
def ROLR   : RRR<0x50C04000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "rolr\t$rd, $rs1, $rs2">;
def RORR_R : RRR<0x51004000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                "rorr\t$rd, $rs1, $rs2">;
}

// Compare-and-materialise: rd = (rs1 cond rs2) ? 1 : 0, flags untouched
// (class 3 B = 1).  Min / max write no flags either.
let Defs = []<Register>, SchedRW = [WriteALU] in {
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
}

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
// because allowsMisalignedMemoryAccesses() allows no misaligned store, so
// the legalizer expands every under-aligned i16/i32 store to byte ops (MEMSET8/
// STIDX8) and never selects a masking store on an unaligned address.  It
// allows only an under-aligned 32-bit *load*, which isel makes a MEMGET32
// (unaligned-tolerant); keep any future permission to the load path too.
// (Same invariant as the force-aligned LDIDX64/STIDX64 note above.)
def MEMSET8  : RRst<0x5C000000, "memset8\t$rs_data, $rs_addr">;
def MEMSET16 : RRst<0x5D000000, "memset16\t$rs_data, $rs_addr">;
def MEMSET32 : RRst<0x5E000000, "memset32\t$rs_data, $rs_addr">;

//===----------------------------------------------------------------------===//
// ALU — unary: rd = f(rs1)
//===----------------------------------------------------------------------===//

def INCR    : R_unary<0x57880000, "incr">;
def DECR    : R_unary<0x57C80000, "decr">;
def NEGR    : R_unary<0x54480000, "negr",
                [(set GPR:$rd, (ineg GPR:$rs1))]>;
def ABSR    : R_unary<0x54C80000, "absr",
                [(set GPR:$rd, (abs GPR:$rs1))]>;
// Shift by 1 with F = 0: no flags (SHLV/SHRV by 1 write Z).
let Defs = []<Register>, SchedRW = [WriteALU] in {
def SHLR1   : R_unary<0x50208000, "shlr1">;
def SHRR1   : R_unary<0x50608000, "shrr1">;
}
def NOTR    : R_unary<0x54880000, "notr",
                [(set GPR:$rd, (not GPR:$rs1))]>;
def POPCNT  : R_unary<0x56080000, "popcnt",
                [(set GPR:$rd, (ctpop GPR:$rs1))]>;
// F = 0: no flags.
let Defs = []<Register>, SchedRW = [WriteALU] in {
def CLZ     : R_unary<0x56400000, "clz",
                [(set GPR:$rd, (ctlz GPR:$rs1))]>;
def CTZ     : R_unary<0x56800000, "ctz",
                [(set GPR:$rd, (cttz GPR:$rs1))]>;
def BITREV  : R_unary<0x55C00000, "bitrev",
                [(set GPR:$rd, (bitreverse GPR:$rs1))]>;
def BSWAP_R : R_unary<0x55800000, "bswap",
                [(set GPR:$rd, (bswap GPR:$rs1))]>;
}

// Sign-extend to 64 bits (arch doc §7.2; hardware extends to 64)
def SEXTB   : R_unary<0x55080000, "sextb",
                [(set GPR:$rd, (sext_inreg GPR:$rs1, i8))]>;
def SEXTH   : R_unary<0x55180000, "sexth",
                [(set GPR:$rd, (sext_inreg GPR:$rs1, i16))]>;

// Zero-extend to 64 bits
def ZEXTB   : R_unary<0x55480000, "zextb">;
def ZEXTH   : R_unary<0x55580000, "zexth">;

// Sign/zero-extend 32→64 bits (hardware fixed April 2026); F = 0, unlike
// the byte and halfword forms: no flags.
let Defs = []<Register>, SchedRW = [WriteALU] in {
def SEXTW   : R_unary<0x55200000, "sextw",
                [(set GPR:$rd, (sext_inreg GPR:$rs1, i32))]>;
def ZEXTW   : R_unary<0x55600000, "zextw">;
}

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
let Defs = [SP], Uses = [SP], hasSideEffects = 1 in {
def ADJCALLSTACKDOWN : Pseudo<(outs), (ins i64imm:$amt1, i64imm:$amt2),
                              [(callseq_start timm:$amt1, timm:$amt2)]>;
def ADJCALLSTACKUP   : Pseudo<(outs), (ins i64imm:$amt1, i64imm:$amt2),
                              [(callseq_end timm:$amt1, timm:$amt2)]>;
}

// SELECT without CMOV: KlaussCPUISD::SELECT_CC (lhs, rhs, cc, t, f) selects
// to one of these; EmitInstrWithCustomInserter (KlaussCPUISelLowering.cpp)
// expands it to CMPRR/CMPRV/CMPRVU + the conditional jump in $jmp + a PHI.
// $jmp holds the JMPxx opcode (from KlaussCPUInstrInfo::getCondBranchOpcode).
// SELECT_CC_RI's $rhs is a plain i64imm, not simm32: it holds a simm32
// (CMPRV / CMPRVW) or a value in [2^31, 2^32) (CMPRVU).
// Defs=[FLAGS]: the expansion's compare clobbers the flags.
let usesCustomInserter = 1, hasSideEffects = 0, Defs = [FLAGS] in {
def SELECT_CC_RR : Pseudo<(outs GPR:$dst),
                          (ins GPR:$lhs, GPR:$rhs, i64imm:$jmp,
                               GPR:$trueV, GPR:$falseV)>;
def SELECT_CC_RI : Pseudo<(outs GPR:$dst),
                          (ins GPR:$lhs, i64imm:$rhs, i64imm:$jmp,
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
// The registers a call clobbers are given by its register-mask operand
// (CSR_KlaussCPU_Call, added by LowerCall); FLAGS too (the callee is free to
// change them).
let isCall = 1,
    Uses = [SP],
    Defs = [FLAGS] in
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

// Constant materialisation: side-effect free and rematerializable, so
// MachineLICM can hoist it, MachineCSE merge it and the register allocator
// rematerialize it instead of spilling it.
let hasSideEffects = 0, isReMaterializable = 1 in {
// Load 32-bit immediate (sign-extended to 64 bits)
let isAsCheapAsAMove = 1, isMoveImm = 1 in
def SETR    : RV_ld<0x8BD00000, "setr\t$rd, $imm", [], simm32_abs>;

// Load 32-bit immediate zero-extended to 64 bits: the class 2 MOV with
// SGN=0 (pipeline_core.sv f_decode_raw `{4'd15,1'b0}` U_MOV, ex_imm and
// exo_result; ISA_ENCODING_V2_MAP §5 "zero-extended MOV").  Constants in
// [2^31, 2^32) and symbol addresses, which are 32-bit and zero-extended
// like LEAPC's (CPU_ARCHITECTURE.md §11).  Not isAsCheapAsAMove, like the
// SETR64 it replaces: MachineLICM then hoists such a constant (mostly an MMIO
// address) out of a loop instead of leaving it in the body to be
// rematerialized there (with the flag, the emulator corpus executed ~9K/14K
// more instructions at -O1/-O2 than with SETR64).
let isMoveImm = 1 in
def SETRU   : RV_ld<0x8BC00000, "setru\t$rd, $imm", [], uimm32_abs>;

// Load full 64-bit immediate: rd = (hi32 << 32) | lo32
let isMoveImm = 1 in
def SETR64  : V64_ld<0xCBC00000, "setr64\t$rd, $lo, $hi">;
}

// Immediate operations (rd = rs op imm32); the CPU zero-extends the
// imm32 of ADDV/MINUSV/ANDV/ORV/XORV and sign-extends the MULV/DIVV/MODV one.
def ADDV    : RV_imm<0x88200000, "addv", uimm32>;
def MINUSV  : RV_imm<0x88600000, "minusv", uimm32>;
// The CPU writes no flags for AND/OR/XOR (no fmask).
let Defs = []<Register>, SchedRW = [WriteALU] in {
def ANDV    : RV_imm<0x89000000, "andv", uimm32>;
def ORV     : RV_imm<0x89400000, "orv", uimm32>;
def XORV    : RV_imm<0x89800000, "xorv", uimm32>;
// The same bitwise ops with a sign-extended imm32 (SGN=1: pipeline_core.sv
// f_decode_raw class 2 `{4'd4..6,1'b0}`, ex_b): a mask with the high 32 bits
// set, e.g. the `& -8` of an alignment round-up.
def ANDI    : RV_imm<0x89100000, "andi">;
def ORI     : RV_imm<0x89500000, "ori">;
def XORI    : RV_imm<0x89900000, "xori">;
}
// Shift by immediate (count = imm[5:0]); v2: single word, count embedded.
def SHLV    : ShiftImmN<0x50204000, "shlv">;
def SHRV    : ShiftImmN<0x50604000, "shrv">;
def SHRAV   : ShiftImmN<0x50A04000, "shrav">;
// The immediate forms use the same EX-serialised multiplier and divider as
// MULR/DIVR (pipeline_core.sv class A decode: U_MUL/U_DIV/U_MOD for LEN=10).
let SchedRW = [WriteMul, WriteMulFlags] in
def MULV    : RV_imm<0xA8800000, "mulv">;
let SchedRW = [WriteDiv, WriteDivFlags] in {
def DIVV    : RV_imm<0xA9800000, "divv">;
def MODV    : RV_imm<0xAA800000, "modv">;
}

// Rotate by immediate (count = imm[5:0]); v2: single word.  Register-amount
// forms are ROLR/RORR_R.
def ROLV    : ShiftImmN<0x50E04000, "rolv">;
def RORV    : ShiftImmN<0x51204000, "rorv">;

// reg+imm32 add: rd = rs + sign_extend(imm32). Two distinct regs; sign-extends.
// Also materialises a frame address in one instruction (see AddrFI).
def ADDI : RRV_alu<0x88300000, "addi\t$rd, $rs, $imm">;

// Compare register to immediate — flags only (see CMPRR_I on side effects).
// CMPRVU_I zero-extends its imm32 (class 3 SGN=0: pipeline_core.sv
// f_decode_raw class 3, id_b_sext / ex_b): a compare with a constant in
// [2^31, 2^32).
let hasSideEffects = 0 in {
def CMPRV_I  : RV_cmp<0x8C100000, "cmprv\t$rs, $imm">;
def CMPRVU_I : RV_cmp<0x8C000000, "cmprvu\t$rs, $imm", uimm32>;
}

//===----------------------------------------------------------------------===//
// Memory: indexed load / store  (primary frame-slot instructions)
//
// Class 6 (load) / 7 (store), MODE=01 (rs1 + imm32): word 0 holds
// SIZE [25:24], SGN [23] and A [20]; rd = destination / store data,
// rs1 = base, word 1 = offset32.  Sub-word loads zero-extend unless SGN=1.
//
// IMPORTANT: ldidx64/stidx64 (0x9B300000/0x9F300000) are the *force-aligned*
// hardware variants (A=1, ISA map LDIDX64A/STIDX64A) — the RTL computes
// (rs1 + imm) & ~7 before the bus access.  The non-aligned forms are
// ldidx64u/stidx64u (A=0).  Using the force-aligned form is only safe because
// LLVM never emits a misaligned i64 access: allowsMisalignedMemoryAccesses()
// is false for i64, so under-aligned i64 loads/stores are expanded by the
// legalizer (loads to two 32-bit MEMGET32 halves, stores to byte ops).  If misaligned i64 access is ever
// enabled, switch these to the A=0 forms first (and confirm the HW handles a
// 64-bit read spanning two cache lines).
//===----------------------------------------------------------------------===//

def LDIDX64 : RRV_ld<0x9B300000, "ldidx64\t$rd, $base, $offset">;
def STIDX64 : RRV_st<0x9F300000, "stidx64\t$rs_data, $base, $offset">;

def LDIDX32 : RRV_ld<0x9A200000, "ldidx32\t$rd, $base, $offset">;
// STIDX32/STIDX16 mask addr&~3 / addr&~1 like MEMSET32/16 — safe only while
// allowsMisalignedMemoryAccesses() allows no misaligned store. See the
// alignment warning above the MEMSET8/16/32 defs.  LDIDX32 masks addr&~3
// too: its patterns take only aligned loads (zextloadi32_a4, ...).
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

// Accept the ISA map's spelling of the signed indexed loads (LDIDX8_S /
// LDIDX16_S / LDIDX32_S) as assembler aliases for the printed mnemonics.
// Mnemonics reach the matcher lower-cased, so the alias keys are lower-case.
def : MnemonicAlias<"ldidx8_s",  "ldidx8s">;
def : MnemonicAlias<"ldidx16_s", "ldidx16s">;
def : MnemonicAlias<"ldidx32_s", "ldidx32s">;

//===----------------------------------------------------------------------===//
// Constant, address and load / store isel patterns
//
// simm32_imm: matches a 64-bit immediate that fits in a signed 32-bit field,
// used as the offset in LDIDX64 / STIDX64.
//===----------------------------------------------------------------------===//

def simm32_imm : ImmLeaf<i64, [{return isInt<32>(Imm);}]>;
// A constant in [2^31, 2^32): an unsigned 32-bit value that no sign-extending
// imm32 reaches (SETRU / ADDV / CMPRVU).
def uimm32_nonsimm32_imm
    : ImmLeaf<i64, [{return isUInt<32>(Imm) && !isInt<32>(Imm);}]>;
// The negation of one, as `add` sees a subtraction of it (MINUSV).
def neg_uimm32_nonsimm32_imm
    : ImmLeaf<i64, [{return Imm < 0 && isUInt<32>(-(uint64_t)Imm) &&
                            !isInt<32>(Imm);}]>;
// Neither a sign- nor a zero-extended imm32 (SETR64).
def imm64_imm
    : ImmLeaf<i64, [{return !isInt<32>(Imm) && !isUInt<32>(Imm);}]>;

// SETR64's halves: rd = (hi32 << 32) | lo32, each printed signed.
def Lo32XForm : SDNodeXForm<imm, [{
  return CurDAG->getTargetConstant(SignExtend64<32>(N->getZExtValue()),
                                   SDLoc(N), MVT::i64);
}]>;
def Hi32XForm : SDNodeXForm<imm, [{
  return CurDAG->getTargetConstant(SignExtend64<32>(N->getZExtValue() >> 32),
                                   SDLoc(N), MVT::i64);
}]>;
def NegImm : SDNodeXForm<imm, [{
  return CurDAG->getTargetConstant(-N->getSExtValue(), SDLoc(N), MVT::i64);
}]>;

// Materialize a constant into a GPR: a signed 32-bit one with SETR, one in
// [2^31, 2^32) with the zero-extending SETRU (both 2 words; their CompressPats
// pick the 1-word form for -128..255), any other with one 12-byte SETR64.
def : Pat<(i64 simm32_imm:$imm), (SETR simm32_imm:$imm)>;
def : Pat<(i64 uimm32_nonsimm32_imm:$imm), (SETRU uimm32_nonsimm32_imm:$imm)>;
def : Pat<(i64 imm64_imm:$imm),
          (SETR64 (Lo32XForm imm:$imm), (Hi32XForm imm:$imm))>;

// Register + 32-bit constant addition in one instruction: ADDI sign-extends
// its imm32, ADDV zero-extends it, and MINUSV subtracts a zero-extended one.
def : Pat<(i64 (add GPR:$rs, simm32_imm:$imm)), (ADDI GPR:$rs, simm32_imm:$imm)>;
def : Pat<(i64 (add GPR:$rs, uimm32_nonsimm32_imm:$imm)),
          (ADDV GPR:$rs, uimm32_nonsimm32_imm:$imm)>;
def : Pat<(i64 (add GPR:$rs, neg_uimm32_nonsimm32_imm:$imm)),
          (MINUSV GPR:$rs, (NegImm imm:$imm))>;
// -255..-129: MINUSV of 129..255 compresses to the 1-word MINUSV_SH, where
// ADDI's short form only reaches -128.  The value, Z, S and V are ADDI's; C
// is the borrow instead of the carry, and nothing reads the C of an `add`.
def neg_uimm8_nonsimm8_imm
    : ImmLeaf<i64, [{return Imm >= -255 && Imm <= -129;}]>;
let AddedComplexity = 1 in
def : Pat<(i64 (add GPR:$rs, neg_uimm8_nonsimm8_imm:$imm)),
          (MINUSV GPR:$rs, (NegImm imm:$imm))>;

// A frame index, or a frame index plus a constant (KlaussCPUDAGToDAGISel::
// selectFrameAddr, which decides when folding the constant pays), as
// (TargetFrameIndex, offset): eliminateFrameIndex rewrites the pair to
// (frame register, slot offset + offset).  As a value (AddrFI) it is one
// ADDI rd, FI, offset; loads and stores take it as their base and offset
// (AddrFIMem, below, which sees the access size).  AddedComplexity puts both
// ahead of the GPR-address patterns, which compute the frame address with a
// separate ADDI.
def AddrFI : ComplexPattern<iPTR, 2, "SelectAddrFI", [frameindex, add, or]>;
def AddrFIMem : ComplexPattern<iPTR, 2, "SelectAddrFIMem"> {
  let WantsParent = true;
}

let AddedComplexity = 10 in
def : Pat<(AddrFI (i64 GPR:$base), simm32_imm:$off),
          (ADDI GPR:$base, simm32_imm:$off)>;

//===----------------------------------------------------------------------===//
// Immediate-form ALU patterns
//
// These fold a constant operand directly into the RV_imm / ShiftImmN
// encoding, avoiding the separate SETR (8 B) that the register-register form
// would need to materialise the constant.  This matters disproportionately on
// KlaussCPU because the core is fetch-bound — fewer fetched words ≈ fewer
// cycles.
//
// Extension semantics (ISA_ENCODING_V2_MAP §2, SGN) gate each pattern —
// getting these wrong would silently miscompile:
//   SHLV/SHRV/SHRAV : shift count is imm[5:0]            → uimm6   (0–63)
//   ANDV/ORV/XORV   : imm32 is ZERO-extended to 64 bits  → uimm32_imm (hi32==0)
//   ANDI/ORI/XORI   : imm32 is SIGN-extended to 64 bits   → negsimm32_imm
//   MULV            : imm32 is SIGN-extended to 64 bits   → simm32_imm
// Out-of-range constants simply fall through to the register-register form.
//===----------------------------------------------------------------------===//

def uimm32_imm : ImmLeaf<i64, [{ return isUInt<32>(Imm); }]>;

// Shift by a constant amount (0–63).
def : Pat<(shl GPR:$rs, uimm6:$sh), (SHLV  GPR:$rs, uimm6:$sh)>;
def : Pat<(srl GPR:$rs, uimm6:$sh), (SHRV  GPR:$rs, uimm6:$sh)>;
def : Pat<(sra GPR:$rs, uimm6:$sh), (SHRAV GPR:$rs, uimm6:$sh)>;

// Shift or rotate by a register amount.  The CPU uses only rs2[5:0]
// (pipeline_core.sv ex_shcnt = ex_b[5:0]; klausscc emu/exec.rs
// exec_shift_bit `& 0x3F`), so selectShiftMask drops what cannot change
// those bits: an `and` that keeps them (the `& 63` of a rotate or funnel
// shift, the zero-extension of a promoted i32 amount) and X +/-/|/^ 64k,
// nested, and turns 64k - X into NEGR and 63 - X (or X ^ 63) into NOTR.
def shamt : ComplexPattern<i64, 1, "selectShiftMask", [], [], 0>;
def : Pat<(shl  GPR:$rs1, shamt:$rs2), (SHLR   GPR:$rs1, shamt:$rs2)>;
def : Pat<(srl  GPR:$rs1, shamt:$rs2), (SHRR   GPR:$rs1, shamt:$rs2)>;
def : Pat<(sra  GPR:$rs1, shamt:$rs2), (SARR   GPR:$rs1, shamt:$rs2)>;
def : Pat<(rotl GPR:$rs1, shamt:$rs2), (ROLR   GPR:$rs1, shamt:$rs2)>;
def : Pat<(rotr GPR:$rs1, shamt:$rs2), (RORR_R GPR:$rs1, shamt:$rs2)>;

// Bitwise op with a mask that fits in unsigned 32 bits (imm is zero-extended).
// Masks that are neither a zero- nor a sign-extended imm32 fall through to
// SETR64 + ANDR/ORR/XORR.
def : Pat<(and GPR:$rs, uimm32_imm:$i), (ANDV GPR:$rs, uimm32_imm:$i)>;
def : Pat<(or  GPR:$rs, uimm32_imm:$i), (ORV  GPR:$rs, uimm32_imm:$i)>;
def : Pat<(xor GPR:$rs, uimm32_imm:$i), (XORV GPR:$rs, uimm32_imm:$i)>;
// A negative simm32 mask (high 32 bits all ones; -1 itself is NOTR).
def negsimm32_imm
    : ImmLeaf<i64, [{ return Imm < 0 && Imm != -1 && isInt<32>(Imm); }]>;
def : Pat<(and GPR:$rs, negsimm32_imm:$i), (ANDI GPR:$rs, negsimm32_imm:$i)>;
def : Pat<(or  GPR:$rs, negsimm32_imm:$i), (ORI  GPR:$rs, negsimm32_imm:$i)>;
def : Pat<(xor GPR:$rs, negsimm32_imm:$i), (XORI GPR:$rs, negsimm32_imm:$i)>;
// A mask of the high 64-k bits (k >= 32) or of the low k bits (k > 32),
// which no imm32 reaches: a shift pair (2 words) instead of SETR64 + ANDR
// (4 words, the same count).  Only for a constant with no other use in the
// block: a shared SETR64 serves every AND with one more instruction.  Not in
// a loop either, where MachineLICM hoists the SETR64 (preferMaskShiftPair).
def highmask64_imm : PatLeaf<(imm), [{
  uint64_t V = N->getZExtValue();
  return N->hasOneUse() && V != 0 && isMask_64(~V) &&
         llvm::countr_zero(V) >= 32 && preferMaskShiftPair();
}]>;
def lowmask64_imm : PatLeaf<(imm), [{
  uint64_t V = N->getZExtValue();
  return N->hasOneUse() && isMask_64(V) && llvm::popcount(V) > 32 &&
         llvm::popcount(V) < 64 && preferMaskShiftPair();
}]>;
def TrailingZeros : SDNodeXForm<imm, [{
  return CurDAG->getTargetConstant(llvm::countr_zero(N->getZExtValue()),
                                   SDLoc(N), MVT::i64);
}]>;
def LeadingZeros : SDNodeXForm<imm, [{
  return CurDAG->getTargetConstant(llvm::countl_zero(N->getZExtValue()),
                                   SDLoc(N), MVT::i64);
}]>;
def : Pat<(and GPR:$rs, highmask64_imm:$m),
          (SHLV (SHRV GPR:$rs, (TrailingZeros imm:$m)), (TrailingZeros imm:$m))>;
def : Pat<(and GPR:$rs, lowmask64_imm:$m),
          (SHRV (SHLV GPR:$rs, (LeadingZeros imm:$m)), (LeadingZeros imm:$m))>;

// Multiply by a signed-32 constant (imm is sign-extended).
def : Pat<(mul GPR:$rs, simm32_imm:$i), (MULV GPR:$rs, simm32_imm:$i)>;

// Boolean compares (class 3 B=1: rd = 0/1, flags untouched) beyond the
// register forms CMPEQR..CMPUGER: the 2-word compare with a sign-extended
// imm32 (`cmpltv rd, rs, imm`: the reg form's template with LEN=10, SGN=1)
// and the ISA v3 D2 32-bit forms, W [19] on the reg form (`cmpltrw`) and the
// 2-word form (`cmpltvw`), which order the low 32 bits of both operands.
// ISA_ENCODING_V2_MAP.md §5 ("boolean compare-immediate (B=1, LEN=10)") and
// §7.3; RTL pipeline_core.sv f_decode_raw class 3 (B=1 -> U_BOOL with
// f_cmp_op(PRED, INV), no fmask), id_b_imm / id_b_sext (imm32, extended per
// SGN), id_w + f_wsh (W: both operands shifted left 32 before the EX
// compare).  The W forms are chosen in C++ (KlaussCPUDAGToDAGISel, as for
// CMPRRW/CMPRVW); the immediate form by the patterns here.  The W forms have
// no pattern, so the properties are stated explicitly: otherwise TableGen
// guesses hasSideEffects = 1 and MachineCSE/LICM/sinking and select grouping
// would all have to step around them.
multiclass BoolCompare<bits<32> rr, string mn, CondCode cc> {
  defvar imm = !or(!and(rr, 0x3FFFFFFF), 0x80100000);
  let Defs = []<Register>, hasSideEffects = 0, mayLoad = 0, mayStore = 0 in {
    def V  : RV_imm<imm, mn # "v">;
    def RW : RRR<!or(rr, 0x00080000), (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2),
                 mn # "rw\t$rd, $rs1, $rs2">;
    def VW : RV_imm<!or(imm, 0x00080000), mn # "vw">;
  }
  def : Pat<(i64 (setcc GPR:$rs, simm32_imm:$imm, cc)),
            (!cast<Instruction>(NAME # "V") GPR:$rs, simm32_imm:$imm)>;
}
defm CMPEQ  : BoolCompare<0x4C200000, "cmpeq",  SETEQ>;
defm CMPNE  : BoolCompare<0x4C600000, "cmpne",  SETNE>;
defm CMPLT  : BoolCompare<0x4CA00000, "cmplt",  SETLT>;
defm CMPGE  : BoolCompare<0x4CE00000, "cmpge",  SETGE>;
defm CMPLE  : BoolCompare<0x4D200000, "cmple",  SETLE>;
defm CMPGT  : BoolCompare<0x4D600000, "cmpgt",  SETGT>;
defm CMPULT : BoolCompare<0x4DA00000, "cmpult", SETULT>;
defm CMPUGE : BoolCompare<0x4DE00000, "cmpuge", SETUGE>;
defm CMPULE : BoolCompare<0x4E200000, "cmpule", SETULE>;
defm CMPUGT : BoolCompare<0x4E600000, "cmpugt", SETUGT>;

// Rotate by a constant amount (0–63) → ROLV/RORV (count = imm[5:0]).
def : Pat<(rotl GPR:$rs, uimm6:$sh), (ROLV GPR:$rs, uimm6:$sh)>;
def : Pat<(rotr GPR:$rs, uimm6:$sh), (RORV GPR:$rs, uimm6:$sh)>;

// A 32-bit rotate is the 64-bit rotate of x[31:0] copied into both halves,
// ORR (SHLV x, 32), zext(x): a rotate of a value that repeats every 32 bits
// leaves the 32-bit rotate in each half, for any amount modulo 32.  Dup32
// gives (x, zext(x)), taking the zero-extension from an explicit
// `and 0xFFFFFFFF`, a load or known-zero upper bits when it can (no ZEXTW),
// and the duplicated value is shared (CSE) by the rotates of one x.
def Dup32 : ComplexPattern<i64, 2, "selectDup32", [], [], 0>;
def : Pat<(KlaussCPURolW (Dup32 GPR:$hi, GPR:$lo), uimm6:$sh),
          (ROLV (ORR (SHLV GPR:$hi, 32), GPR:$lo), uimm6:$sh)>;
def : Pat<(KlaussCPURorW (Dup32 GPR:$hi, GPR:$lo), uimm6:$sh),
          (RORV (ORR (SHLV GPR:$hi, 32), GPR:$lo), uimm6:$sh)>;
def : Pat<(KlaussCPURolW (Dup32 GPR:$hi, GPR:$lo), shamt:$sh),
          (ROLR (ORR (SHLV GPR:$hi, 32), GPR:$lo), shamt:$sh)>;
def : Pat<(KlaussCPURorW (Dup32 GPR:$hi, GPR:$lo), shamt:$sh),
          (RORR_R (ORR (SHLV GPR:$hi, 32), GPR:$lo), shamt:$sh)>;

// (x & 0xFFFFFFFF) is exactly a 32→64 zero-extend → single 4-byte ZEXTW.
// AddedComplexity beats the ANDV pattern above for this specific mask.
let AddedComplexity = 3 in
def : Pat<(and GPR:$rs, 0xFFFFFFFF), (ZEXTW GPR:$rs)>;

// (x & 0xFF) / (x & 0xFFFF) → single 4-byte ZEXTB/ZEXTH instead of 8-byte ANDV.
let AddedComplexity = 3 in {
def : Pat<(and GPR:$rs, 0xFF),   (ZEXTB GPR:$rs)>;
def : Pat<(and GPR:$rs, 0xFFFF), (ZEXTH GPR:$rs)>;
}

// INCR/DECR: add ±1 as a single 4-byte op that also sets Z (consumed
// by the KlaussCPUFlagReuse peephole).  AddedComplexity so they beat ADDV/ADDI.
let AddedComplexity = 2 in {
def : Pat<(add GPR:$rs,  1), (INCR GPR:$rs)>;
def : Pat<(add GPR:$rs, -1), (DECR GPR:$rs)>;
}

// Shift-by-1 needs no special case under v2: SHLV/SHRV with an embedded count
// are single-word (they were 2-word in v1, which is the only reason SHLR1/SHRR1
// were preferred then).  `(shl x, 1)` / `(srl x, 1)` fold into the general
// SHLV/SHRV patterns above; the delta from SHLR1/SHRR1 is that SHLV/SHRV
// also set the Z flag (F = 1), so they define FLAGS where SHLR1/SHRR1 do not.
// Rotate-by-1 lowers through ROLV/RORV, byte-identical to ROLR1/RORR1 (Z and
// C).  SHLR1/SHRR1 remain as assembler-only mnemonics.

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

// 32-bit loads.  Only MEMGET32 ([r]) reads an unaligned word
// (pipeline_core.sv:1001, :1187, :1230-1234, :1351-1366); every other 32-bit
// load force-aligns its EA (:1189).  allowsMisalignedMemoryAccesses lets an
// under-aligned non-volatile 32-bit load reach isel, so the base + offset,
// reg + reg and frame-index forms take only naturally aligned ones; an
// under-aligned one is MEMGET32 of the computed address (+ SEXTW: MEMGET32
// has no sign-extending form, :564).
let IsLoad = 1, MinAlignment = 4 in {
def zextloadi32_a4 : PatFrag<(ops node:$ptr), (zextloadi32 node:$ptr)>;
def extloadi32_a4  : PatFrag<(ops node:$ptr), (extloadi32 node:$ptr)>;
def sextloadi32_a4 : PatFrag<(ops node:$ptr), (sextloadi32 node:$ptr)>;
}
def sextloadi32_unaligned : PatFrag<(ops node:$ptr), (sextloadi32 node:$ptr), [{
  return cast<LoadSDNode>(N)->getAlign() < Align(4);
}]>;

// Sub-word zero-extending loads from a plain GPR address (no offset).
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
def : Pat<(i64 (zextloadi32_a4 (add (i64 GPR:$base), simm32_imm:$off))),
          (LDIDX32 GPR:$base, simm32_imm:$off)>;
def : Pat<(i64 (extloadi32_a4  (add (i64 GPR:$base), simm32_imm:$off))),
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

// Loads and stores off a frame index plus a constant (AddrFIMem), for every
// width and extension: base+offset with the frame register as the base.
multiclass FrameLoadPat<PatFrag ld, Instruction Inst> {
  def : Pat<(i64 (ld (AddrFIMem (i64 GPR:$base), simm32_imm:$off))),
            (Inst GPR:$base, simm32_imm:$off)>;
}
multiclass FrameStorePat<PatFrag st, Instruction Inst> {
  def : Pat<(st (i64 GPR:$val), (AddrFIMem (i64 GPR:$base), simm32_imm:$off)),
            (Inst GPR:$val, GPR:$base, simm32_imm:$off)>;
}
let AddedComplexity = 10 in {
defm : FrameLoadPat<load,        LDIDX64>;
defm : FrameLoadPat<zextloadi32_a4, LDIDX32>;
defm : FrameLoadPat<extloadi32_a4,  LDIDX32>;
defm : FrameLoadPat<sextloadi32_a4, LDIDX32_S>;
defm : FrameLoadPat<zextloadi16, LDIDX16>;
defm : FrameLoadPat<extloadi16,  LDIDX16>;
defm : FrameLoadPat<sextloadi16, LDIDX16_S>;
defm : FrameLoadPat<zextloadi8,  LDIDX8>;
defm : FrameLoadPat<extloadi8,   LDIDX8>;
defm : FrameLoadPat<sextloadi8,  LDIDX8_S>;
defm : FrameStorePat<store,         STIDX64>;
defm : FrameStorePat<truncstorei32, STIDX32>;
defm : FrameStorePat<truncstorei16, STIDX16>;
defm : FrameStorePat<truncstorei8,  STIDX8>;
}

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

// Clobbers: the call's register mask (CSR_KlaussCPU_Call), and FLAGS.
let Uses = [SP],
    Defs = [FLAGS] in
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

// PC-relative conditional branches: the conditions of their absolute twins,
// derived from the one Z/S/C/V flag set (EQ = Z, LT = S^V, ULT = C, ...).
def JMPZREL   : Vbr_relcond<0xA1080000, "jmpzrel\t$target">;
def JMPNZREL  : Vbr_relcond<0xA10C0000, "jmpnzrel\t$target">;
def JMPEREL   : Vbr_relcond<0xA1480000, "jmperel\t$target">;
def JMPNEREL  : Vbr_relcond<0xA14C0000, "jmpnerel\t$target">;
def JMPCREL   : Vbr_relcond<0xA1100000, "jmpcrel\t$target">;
def JMPNCREL  : Vbr_relcond<0xA1140000, "jmpncrel\t$target">;
def JMPSREL   : Vbr_relcond<0xA1200000, "jmpsrel\t$target">;
def JMPNSREL  : Vbr_relcond<0xA1240000, "jmpnsrel\t$target">;
def JMPLTREL  : Vbr_relcond<0xA1280000, "jmpltrel\t$target">;
def JMPLEREL  : Vbr_relcond<0xA1300000, "jmplerel\t$target">;
def JMPGTREL  : Vbr_relcond<0xA1340000, "jmpgtrel\t$target">;
def JMPGEREL  : Vbr_relcond<0xA12C0000, "jmpgerel\t$target">;
def JMPULTREL : Vbr_relcond<0xA1380000, "jmpultrel\t$target">;
def JMPULEREL : Vbr_relcond<0xA1400000, "jmpulerel\t$target">;
def JMPUGTREL : Vbr_relcond<0xA1440000, "jmpugtrel\t$target">;
def JMPUGEREL : Vbr_relcond<0xA13C0000, "jmpugerel\t$target">;

// Tail-call jump (PseudoTAIL): JMPREL's encoding, spelled `tail` so that a
// jump to a symbol compresses to TAIL_SH, the short jump with a short call's
// fixup.  The disassembler shows both as jmprel.
let isAsmParserOnly = 1 in
def TAILREL : Vbr_uncond<0xA1000000, "tail\t$target", [], PCRelCallTarget32>;

// PC-relative direct call: push PC+8 (return addr), PC += sign_ext(imm32)
let Uses = [SP],
    Defs = [FLAGS] in
def CALLREL : Vcall<0xA3000000, "callrel\t$target", PCRelCallTarget32>;

// LEAPC: rd = PC_of_instruction + sign_ext(imm32)  (address materialisation)
// Class 2 LEA: word 1 = simm32, FK_KlaussCPU_PCREL32 for a symbol.
let hasSideEffects = 0, isReMaterializable = 1 in
def LEAPC   : RV_ld<0x8B800000, "leapc\t$rd, $imm", [], pcrel_simm32>;

// Symbol addresses (see KlaussCPUAddr).
// A non-PIC address is zero-extended (SETRU) like a PIC one (LEAPC builds
// {32'b0, PC + imm32}) and a 32-bit integer pointer constant.
foreach sym = [tglobaladdr, texternalsym, tblockaddress, tjumptable] in {
def : Pat<(KlaussCPUAddr sym:$sym), (SETRU sym:$sym)>;
def : Pat<(KlaussCPULeaPC sym:$sym), (LEAPC sym:$sym)>;
}

//===----------------------------------------------------------------------===//
// Special / system
//===----------------------------------------------------------------------===//

let isReturn = 1, isTerminator = 1, isBarrier = 1 in
def RET_I   : I0<0x65800000, (outs), (ins), "ret", [(KlaussCPURetGlue)]>;
def NOP_I   : I0<0x6C000000, (outs), (ins), "nop">;
def HALT_I  : I0<0x6C010000, (outs), (ins), "halt">;
// IRET restores the flags the interrupt saved (all four).
let Defs = [FLAGS] in
def IRET_I  : I0<0x65C00000, (outs), (ins), "iret">;

// TRAP — software trap (v2 class B, OP 4: 0x6C040000).  The core parks
// (park_kind 1) and the SoC produces the ERR_TRAP crash dump; the emulator
// stops with `stop = Trap`.  With a debugger attached the core halts at the
// TRAP's address instead (SIGTRAP).  __builtin_trap and __builtin_debugtrap
// both select it; a program's normal end is HALT (the runtime's crt0/_exit).
let hasSideEffects = 1 in
def TRAP_I  : I0<0x6C040000, (outs), (ins), "trap">;

// WAIT — interruptible core-suspend: stop fetching/executing and resume when an
// unmasked, vectored interrupt becomes pending (timer tick, future wired source).
// No register/flag side effects; IRET resumes at PC+4.  Distinct from HALT (which
// stops permanently).  hasSideEffects keeps it from being reordered/eliminated.
let hasSideEffects = 1 in
def WAIT_I  : I0<0x6C020000, (outs), (ins), "wait">;

//===----------------------------------------------------------------------===//
// Assembler-only opcodes
//
// These instructions are NOT emitted by the code generator (no isel Pattern);
// they exist so hand-written `.s` / inline-asm can name the rest of the ISA
// (ISA_ENCODING_V2_MAP.md).  The LED / 7-segment / RGB opcodes of ISA v1 no
// longer exist (those peripherals, like the UART, are memory-mapped), so
// they are not defined: their mnemonics are assembler errors.
//===----------------------------------------------------------------------===//

// ── RRR (rd, rs1, rs2) ─────────────────────────────────────────────────────
def ADDC   : RRR<0x44A00000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "addc\t$rd, $rs1, $rs2"> { let Uses = [FLAGS]; } // reads carry-in
def SUBC   : RRR<0x44E00000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "subc\t$rd, $rs1, $rs2"> { let Uses = [FLAGS]; } // reads borrow-in
// The bit ops by register position have F = 0: no flags.
let Defs = []<Register>, SchedRW = [WriteALU] in {
def BSETRR : RRR<0x52000000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "bsetrr\t$rd, $rs1, $rs2">;
def BCLRRR : RRR<0x52400000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "bclrrr\t$rd, $rs1, $rs2">;
def BTGLRR : RRR<0x52800000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "btglrr\t$rd, $rs1, $rs2">;
def BTSTRR : RRR<0x52C00000, (outs GPR:$rd), (ins GPR:$rs1, GPR:$rs2), "btstrr\t$rd, $rs1, $rs2">;
}

// ── R in-place (rd = f(rd); 2-address) ─────────────────────────────────────
// GETF: rd = {Z, Z, C, V} << 60.  It reads the flags and, with F = 0,
// writes none.
let Defs = []<Register>, Uses = [FLAGS], SchedRW = [WriteALU] in
def SETFR : R_inplace<0x57000000, "setfr\t$rd">;
let Defs = []<Register>, SchedRW = [WriteALU] in {
// Same encoding as SHLR1, which is the spelling the disassembler prints.
let isAsmParserOnly = 1 in
def SHLAR : R_unary<0x50208000, "shlar">;
def SHRAR : R_unary<0x50A08000, "shrar">;
}
def ROLR1 : R_unary<0x50E0C000, "rolr1">;
def RORR1 : R_unary<0x5120C000, "rorr1">;
def ROLCR : R_unary<0x5160C000, "rolcr"> { let Uses = [FLAGS]; } // rotate through carry
def RORCR : R_unary<0x51A0C000, "rorcr"> { let Uses = [FLAGS]; } // rotate through carry

// ── Bit ops by embedded position N, bit-field extract / deposit ────────────
// Bit set/clear/toggle by embedded position N — v2: single word; F = 0,
// no flags.
let Defs = []<Register>, SchedRW = [WriteALU] in {
def BSET  : ShiftImmN<0x52200000, "bset">;
def BCLR  : ShiftImmN<0x52600000, "bclr">;
def BTGL  : ShiftImmN<0x52A00000, "btgl">;
}
// Bit-field extract / deposit: 2 words, the field descriptor in imm32
// (start [4:0], len [12:8]; 32-bit fields, result's upper half zero).
//   BEXTR rd, rs1, imm       rd = (rs1 >> start) & mask(len)   (sets Z)
//   BDEP  rd, rs1, rs2, imm  rd = rs1 with rs2[len-1:0] deposited at start
// BEXTR's rs2 field must be 0 (the template's), which the CPU checks.
def BEXTR : RV_imm<0x93000000, "bextr", uimm32>;
def BDEP  : RRRV<0x93400000, "bdep", uimm32>;

// Bit-field extract, (x >> start) & (2^len - 1) and the (x & M) >> start
// form of a zero-extended i32 shift, with start and len in [1, 31]: one
// BEXTR (selectBitFieldExtract) instead of a shift and a mask.  The field
// may take bits from x[63:32] (the CPU shifts the full register:
// pipeline_core.sv U_BEXTR, klausscc emu/exec.rs exec_bit_field).
def BitFieldExtract : ComplexPattern<i64, 2, "selectBitFieldExtract",
                                     [and, srl]>;
let AddedComplexity = 5 in
def : Pat<(BitFieldExtract GPR:$rs, uimm32:$field),
          (BEXTR GPR:$rs, uimm32:$field)>;
// The fields BEXTR cannot take, as a shift pair when the mask would take a
// 2-word ANDV or a SETR64 (selectFieldShiftPair).
def FieldShiftPair : ComplexPattern<i64, 3, "selectFieldShiftPair", [and]>;
let AddedComplexity = 5 in
def : Pat<(FieldShiftPair GPR:$rs, uimm6:$shl, uimm6:$srl),
          (SHRV (SHLV GPR:$rs, uimm6:$shl), uimm6:$srl)>;

// Bit-test by embedded position N — v2: single word, flag-only.
def BTST  : BitTestImmN<0x52E00000, "btst\t$rs, $imm">;

// ── RR (rd, rs) load / (rs_data, rs_addr) store — 8-byte aligned ────────────
def MEMREADRR  : RR1<0x5B000000, (outs GPR:$rd), (ins GPR:$rs), "memreadrr\t$rd, $rs"> { let mayLoad = 1; }
def MEMSET64RR : RRst<0x5F000000, "memset64rr\t$rs_data, $rs_addr">;

// ── RV absolute memory access (rd/rs, address-imm32) — symbol-capable ───────
// MODE=10, EA = imm32 (2 words, no short form), at every width: the ISA map
// names only the 64-bit pair (A=0); the sub-word ones are its word-0 layout
// with SIZE [25:24] and, for loads, SGN [23] (CPU_ARCHITECTURE.md §8.1;
// pipeline_core.sv:549-570 decodes LEN=10 MODE=10 with rs1/rs2 = 0 at every
// SIZE, EA = ex_var1 at :1003, the 16/32-bit forms align it down at
// :1105-1113 and :1186-1188 as LDIDX16/32 do, the load extends per SGN at
// :1371-1373).  Selected for a load or store at a constant address (the
// AbsAddr patterns below).
let hasSideEffects = 0, mayLoad = 1, mayStore = 0, SchedRW = [WriteLoad] in {
def MEMREADR  : RV_ld<0x9B400000, "memreadr\t$rd, $imm", [], uimm32_abs>;
def LDABS8    : RV_ld<0x98400000, "ldabs8\t$rd, $imm", [], uimm32_abs>;
def LDABS16   : RV_ld<0x99400000, "ldabs16\t$rd, $imm", [], uimm32_abs>;
def LDABS32   : RV_ld<0x9A400000, "ldabs32\t$rd, $imm", [], uimm32_abs>;
def LDABS8_S  : RV_ld<0x98C00000, "ldabs8s\t$rd, $imm", [], uimm32_abs>;
def LDABS16_S : RV_ld<0x99C00000, "ldabs16s\t$rd, $imm", [], uimm32_abs>;
def LDABS32_S : RV_ld<0x9AC00000, "ldabs32s\t$rd, $imm", [], uimm32_abs>;
}
let hasSideEffects = 0, mayLoad = 0, mayStore = 1, SchedRW = [WriteStore] in {
def MEMSETR  : RV_st<0x9F400000, "memsetr\t$rs_data, $addr">;
def STABS8   : RV_st<0x9C400000, "stabs8\t$rs_data, $addr">;
def STABS16  : RV_st<0x9D400000, "stabs16\t$rs_data, $addr">;
def STABS32  : RV_st<0x9E400000, "stabs32\t$rs_data, $addr">;
}

// A load or store at a constant address (MMIO registers, `*(T *)0x...`) is
// one absolute access instead of SETRU/SETR + MEMGETn/MEMSETn: one
// instruction and 2 words instead of 2 and 3, and no address register.  The
// CPU uses only bits [31:0] of a register address (pipeline_core.sv:1001-1004),
// so a constant that SETR sign-extends is the same address as its low half.
// Only a naturally aligned constant: the 16/32-bit absolute forms align the
// EA down where MEMGET32 does not, and an i64 one is A=0 where LDIDX64 is
// A=1, which agree only on an aligned address.
class AbsAddr<int bytes>
    : ImmLeaf<i64, "return (isInt<32>(Imm) || isUInt<32>(Imm)) && "
                   "(Imm & " # !sub(bytes, 1) # ") == 0;">;
def AbsAddr8  : AbsAddr<1>;
def AbsAddr16 : AbsAddr<2>;
def AbsAddr32 : AbsAddr<4>;
def AbsAddr64 : AbsAddr<8>;
def AbsAddrXForm : SDNodeXForm<imm, [{
  return CurDAG->getTargetConstant(N->getZExtValue() & 0xFFFFFFFF, SDLoc(N),
                                   MVT::i64);
}]>;
multiclass AbsLoadPat<PatFrags ld, ImmLeaf addr, Instruction Inst> {
  def : Pat<(i64 (ld addr:$a)), (Inst (AbsAddrXForm imm:$a))>;
}
multiclass AbsStorePat<PatFrags st, ImmLeaf addr, Instruction Inst> {
  def : Pat<(st (i64 GPR:$val), addr:$a),
            (Inst GPR:$val, (AbsAddrXForm imm:$a))>;
}
let AddedComplexity = 10 in {
defm : AbsLoadPat<load,        AbsAddr64, MEMREADR>;
defm : AbsLoadPat<zextloadi32, AbsAddr32, LDABS32>;
defm : AbsLoadPat<extloadi32,  AbsAddr32, LDABS32>;
defm : AbsLoadPat<sextloadi32, AbsAddr32, LDABS32_S>;
defm : AbsLoadPat<zextloadi16, AbsAddr16, LDABS16>;
defm : AbsLoadPat<extloadi16,  AbsAddr16, LDABS16>;
defm : AbsLoadPat<sextloadi16, AbsAddr16, LDABS16_S>;
defm : AbsLoadPat<zextloadi8,  AbsAddr8,  LDABS8>;
defm : AbsLoadPat<extloadi8,   AbsAddr8,  LDABS8>;
defm : AbsLoadPat<sextloadi8,  AbsAddr8,  LDABS8_S>;
defm : AbsStorePat<store,         AbsAddr64, MEMSETR>;
defm : AbsStorePat<truncstorei32, AbsAddr32, STABS32>;
defm : AbsStorePat<truncstorei16, AbsAddr16, STABS16>;
defm : AbsStorePat<truncstorei8,  AbsAddr8,  STABS8>;
}

// ── RRV 64-bit indexed: non-aligned (ISA map LDIDX64/STIDX64, A=0) and
//    register-offset (LDIDX64R/STIDX64R).  NOTE: LLVM's ldidx64/stidx64 are
//    the *force-aligned* forms (ISA map LDIDX64A/STIDX64A); aliases below.
//    For the *R variants the offset is a second register (EA = base + offset).
def LDIDX64U : RRV_ld<0x9B200000, "ldidx64u\t$rd, $base, $offset">;
def STIDX64U : RRV_st<0x9F200000, "stidx64u\t$rs_data, $base, $offset">;
// v2: register-offset indexed ops are a single word (offset reg in rs2[3:0]).
def LDIDX64R : IdxRegOff_ld<0x5B600000, "ldidx64r\t$rd, $base, $offset">;
def STIDX64R : IdxRegOff_st<0x5F600000, "stidx64r\t$rs_data, $base, $offset">;
// The sub-word MODE=11 forms: the same word-0 layout with SIZE [25:24] and,
// for loads, SGN [23] (CPU_ARCHITECTURE.md §8.1: every SIZE x SGN x MODE is
// legal; pipeline_core.sv:550-570 decodes them, EA = rs1 + rs2 at :1004).
// The ISA map names only the 64-bit pair.  Like the LDIDX16/32 / STIDX16/32
// forms, the 16- and 32-bit ones are force-aligned (EA & ~1 / & ~3; the RTL
// at :1106-1113 and :1186-1188), which is safe while LLVM emits no
// under-aligned access (see the MEMSET16/32 warning above).
def LDIDX8R   : IdxRegOff_ld<0x58600000, "ldidx8r\t$rd, $base, $offset">;
def LDIDX16R  : IdxRegOff_ld<0x59600000, "ldidx16r\t$rd, $base, $offset">;
def LDIDX32R  : IdxRegOff_ld<0x5A600000, "ldidx32r\t$rd, $base, $offset">;
def LDIDX8_SR  : IdxRegOff_ld<0x58E00000, "ldidx8sr\t$rd, $base, $offset">;
def LDIDX16_SR : IdxRegOff_ld<0x59E00000, "ldidx16sr\t$rd, $base, $offset">;
def LDIDX32_SR : IdxRegOff_ld<0x5AE00000, "ldidx32sr\t$rd, $base, $offset">;
def STIDX8R   : IdxRegOff_st<0x5C600000, "stidx8r\t$rs_data, $base, $offset">;
def STIDX16R  : IdxRegOff_st<0x5D600000, "stidx16r\t$rs_data, $base, $offset">;
def STIDX32R  : IdxRegOff_st<0x5E600000, "stidx32r\t$rs_data, $base, $offset">;

// ── V (fixed op32 + imm32/address) — push / conditional jumps ───────────────
def PUSHV : Vsp<0xA4400000, "pushv\t$imm", uimm32> { let mayStore = 1; }
def JMPO  : Vbr_cond<0xA0180000, "jmpo\t$target">;
def JMPNO : Vbr_cond<0xA01C0000, "jmpno\t$target">;

// ── Vcall (conditional calls) ───────────────────────────────────────────────
// They read the flags.  Assembler only: no isel pattern, so no register mask
// says what they clobber; code that uses them must assume a call's clobbers.
let Uses = [SP, FLAGS],
    Defs = [FLAGS] in {
def CALLZ  : Vcall<0xA2080000, "callz\t$target">;
def CALLNZ : Vcall<0xA20C0000, "callnz\t$target">;
def CALLE  : Vcall<0xA2480000, "calle\t$target">;
def CALLNE : Vcall<0xA24C0000, "callne\t$target">;
def CALLC  : Vcall<0xA2100000, "callc\t$target">;
def CALLNC : Vcall<0xA2140000, "callnc\t$target">;
def CALLO  : Vcall<0xA2180000, "callo\t$target">;
def CALLNO : Vcall<0xA21C0000, "callno\t$target">;
}

// ── V64 (push 64-bit immediate; 12-byte) ────────────────────────────────────
def PUSHV64 : V64_imm<0xE4400000, "pushv64\t$lo, $hi"> { let mayStore = 1; }

// ── Delay and LCD ops (side-effecting) ──────────────────────────────────────
let hasSideEffects = 1 in {
  // R_in (single source register)
  def DELAYR   : R_in<0x6C050000, (ins GPR:$reg), "delayr\t$reg">;
  def LCDDATAR : R_in<0x71000000, (ins GPR:$reg), "lcddatar\t$reg">;
  // V (fixed op32 + imm32)
  def DELAYV   : Vsp<0xAC050000, "delayv\t$imm", imm32>;
  def LCD      : Vsp<0xB2000000, "lcd\t$imm", imm32>;   // ISA map LCDRST
  def LCDCMDV  : Vsp<0xB0000000, "lcdcmdv\t$imm", imm32>;
  def LCDDATAV : Vsp<0xB1000000, "lcddatav\t$imm", imm32>;
}

// ── Mnemonic aliases for the ISA map's spellings ────────────────────────────
// The force-aligned 64-bit indexed ops (ISA map LDIDX64A/STIDX64A) are
// LLVM's ldidx64/stidx64.
def : MnemonicAlias<"ldidx64a", "ldidx64">;
def : MnemonicAlias<"stidx64a", "stidx64">;

//===----------------------------------------------------------------------===//
// Call patterns — matched from KlaussCPUISD::CALL nodes emitted by LowerCall.
//===----------------------------------------------------------------------===//

// Direct call to a global address or external symbol: CALL_I, or CALLREL
// under PIC; either is emitted as the short CALL_SH (its CompressPats).  The
// register operands LowerCall attaches (the node is variadic) become the
// call's implicit uses.
// Callee is i64 (global address zero-extended into a GPR; encoder takes [31:0]).
foreach func = [tglobaladdr, texternalsym] in {
let Predicates = [NotPIC] in
def : Pat<(KlaussCPUCallNode (i64 func:$func)), (CALL_I func:$func)>;
let Predicates = [IsPIC] in
def : Pat<(KlaussCPUCallNode (i64 func:$func)), (CALLREL func:$func)>;
}

// Indirect call via register (function pointer).
def : Pat<(KlaussCPUCallNode (i64 GPR:$func)),
          (CALLR_R GPR:$func)>;

// Tail calls (LowerCall, isEligibleForTailCallOptimization): a return that
// jumps to the callee.  emitEpilogue puts the epilogue (LEAVE) in front, so
// the callee is entered with the caller's return address at [SP] and returns
// to the caller's caller.  The AsmPrinter emits PseudoTAIL as TAILREL (which
// becomes the 1-word TAIL_SH, as a call becomes CALL_SH) and
// PseudoTAILIndirect as JMPR.  An indirect callee is in a GPRTC
// register: the epilogue reloads R4-R7, so it cannot be one of them.
let isCall = 1, isTerminator = 1, isReturn = 1, isBarrier = 1,
    isCodeGenOnly = 1, Uses = [SP] in {
def PseudoTAIL : Pseudo<(outs), (ins CallTarget32:$dst), []>;
def PseudoTAILIndirect : Pseudo<(outs), (ins GPRTC:$rs), []>;
}
foreach func = [tglobaladdr, texternalsym] in
def : Pat<(KlaussCPUTailNode (i64 func:$dst)), (PseudoTAIL func:$dst)>;
def : Pat<(KlaussCPUTailNode (i64 GPRTC:$rs)), (PseudoTAILIndirect GPRTC:$rs)>;

// Unconditional and indirect branches.  BR_CC (compare glued to a
// conditional jump, with the W-compare choice) is selected in
// KlaussCPUDAGToDAGISel::Select.
let Predicates = [NotPIC] in
def : Pat<(br bb:$target), (JMP bb:$target)>;
let Predicates = [IsPIC] in
def : Pat<(br bb:$target), (JMPREL bb:$target)>;
def : Pat<(brind GPR:$reg), (JMPR_R GPR:$reg)>;

// __builtin_trap is an abnormal stop and __builtin_debugtrap the debugger's
// software breakpoint: both are TRAP (crash dump, or a debug halt under the
// debugger).  HALT is the clean stop a program ends with, not a trap.
def : Pat<(trap), (TRAP_I)>;
def : Pat<(debugtrap), (TRAP_I)>;

// ISA v3 D3: (sext_inreg (add|sub|mul a, b), i32) → the W op, which drops
// the SEXTW.  Only when the inner op has no other use (else it is computed
// anyway and the SEXTW costs nothing extra).  An add of a simm32 constant
// takes ADDIW (the simm32_imm leaf checks the constant; TableGen drops the
// commuted variant with an immediate on the LHS, as the DAG puts constants
// in operand 1); the register forms take no constant operand (any other
// constant keeps the 64-bit op and its SEXTW).
def w32_addi : PatFrag<(ops node:$a, node:$b), (add node:$a, node:$b), [{
  return N->hasOneUse();
}]>;
class W32RegOp<SDNode op>
    : PatFrag<(ops node:$a, node:$b), (op node:$a, node:$b), [{
  return N->hasOneUse() && !isa<ConstantSDNode>(N->getOperand(0)) &&
         !isa<ConstantSDNode>(N->getOperand(1));
}]>;
def w32_add : W32RegOp<add>;
def w32_sub : W32RegOp<sub>;
def w32_mul : W32RegOp<mul>;

let Predicates = [UseW32Alu] in {
def : Pat<(sext_inreg (w32_addi GPR:$rs, simm32_imm:$imm), i32),
          (ADDIW GPR:$rs, simm32_imm:$imm)>;
def : Pat<(sext_inreg (w32_add GPR:$rs1, GPR:$rs2), i32),
          (ADDW GPR:$rs1, GPR:$rs2)>;
def : Pat<(sext_inreg (w32_sub GPR:$rs1, GPR:$rs2), i32),
          (SUBW GPR:$rs1, GPR:$rs2)>;
def : Pat<(sext_inreg (w32_mul GPR:$rs1, GPR:$rs2), i32),
          (MULW GPR:$rs1, GPR:$rs2)>;
}

// Sign-extending byte/halfword loads — pure GPR address (no base+offset needed).
def : Pat<(i64 (sextloadi8  (i64 GPR:$addr))), (LDIDX8_S  GPR:$addr, 0)>;
def : Pat<(i64 (sextloadi16 (i64 GPR:$addr))), (LDIDX16_S GPR:$addr, 0)>;

// Sign-extending byte/halfword loads — GPR base + signed-32 offset.
def : Pat<(i64 (sextloadi8  (add (i64 GPR:$base), simm32_imm:$off))),
          (LDIDX8_S  GPR:$base, simm32_imm:$off)>;
def : Pat<(i64 (sextloadi16 (add (i64 GPR:$base), simm32_imm:$off))),
          (LDIDX16_S GPR:$base, simm32_imm:$off)>;

// ISA v3 D1: sign-extending 32-bit load (replaces LDIDX32 + SEXTW).
def : Pat<(i64 (sextloadi32_a4 (i64 GPR:$addr))), (LDIDX32_S GPR:$addr, 0)>;
def : Pat<(i64 (sextloadi32_a4 (add (i64 GPR:$base), simm32_imm:$off))),
          (LDIDX32_S GPR:$base, simm32_imm:$off)>;
def : Pat<(i64 (sextloadi32_unaligned (i64 GPR:$addr))),
          (SEXTW (MEMGET32 GPR:$addr))>;

//===----------------------------------------------------------------------===//
// ISA v3 short 1-word forms (ISA_V3_PROPOSAL.md §3, A3/A4/A5).
//
// Never selected by isel.  The CompressPats below rewrite the 2-word form
// into its short twin at MC emission -- from the AsmPrinter and the AsmParser
// alike -- when the immediate fits the 8-bit word-0 field (loads/stores:
// scaled by the access size).  Operand lists and asm text match the long form
// exactly, so the rewrite is an opcode swap.  FeatureShortForms gates it
// (-mllvm -klausscpu-short-forms=false turns it off).
//
// Encoding (ISA_ENCODING_V2_MAP §7.1): the long twin's word 0 with LEN=01
// and the 8-bit immediate in [19:12]; a load/store offset is stored divided
// by the access size (1 << shift).
//===----------------------------------------------------------------------===//

// The imm8 operands: signed or unsigned as the long twin extends its imm32.
// A load/store offset is a byte offset: simm8_lslN is bits [N+7:N] of an
// (N+8)-bit signed offset whose low N bits are zero.  `fits` is checked on
// the long form's operand, of which the encoding keeps the low 32 bits.  Only
// a literal compresses: a symbol needs the long form's word-1 fixup.
class ShortImmOperand<string decoder, string fits>
    : Operand<i64>, ImmLeaf<i64, "return " # fits # ";"> {
  let DecoderMethod = decoder;
  let MCOperandPredicate = [{
    if (!MCOp.isImm())
      return false;
    int64_t Imm = MCOp.getImm();
    return }] # fits # [{;
  }];
}
def uimm8 : ShortImmOperand<"decodeUImmOperand<8>",
                            "isUInt<8>(Lo_32(Imm))">;
foreach shift = 0-3 in
def simm8_lsl # shift
    : ShortImmOperand<"decodeSImmOperand<" # !add(8, shift) # ">",
                      "isShiftedInt<8, " # shift # ">(SignExtend64<32>(Imm))">;

// The short forms are decoded and printed like any instruction, but the
// assembler never matches them (their asm string is the long form's, which
// the matcher picks): AsmVariantName names no parser variant.  They exist
// only with FeatureShortForms: the disassembler does not decode one without
// it, and the MC predicate verifier rejects one reaching the encoder.
class ShortForm<KCInst64 long, dag outs, dag ins, string asmstr>
    : KCInst32<{0b01, long.Word0{29-0}}, outs, ins, asmstr> {
  let AsmVariantName = "NoMatch";
  let Predicates = [HasShortForms];
}

// rd = imm8 (SETR, SETRU)
class ShortRdImm<KCInst64 long, string asmstr, Operand immop = simm8_lsl0>
    : ShortForm<long, (outs GPR:$rd), (ins immop:$imm), asmstr> {
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

// A1 short PC-relative branch or call: the absolute 2-word branch's word 0
// with LEN=01 and REL=1 [24], keeping LINK [25] and COND/INV [22:18]; simm18
// word displacement in [17:0].
class ShortBranch<KCInst64 long, string asmstr, Operand tgt = PCRelTarget18,
                  Predicate pred = HasShortBranches>
    : KCInst32<{0b01, long.Word0{29-25}, 0b1, long.Word0{23-18},
                0b000000000000000000},
               (outs), (ins tgt:$target), asmstr> {
  bits<20> target;
  let Inst{17-0} = target{19-2};
  let AsmVariantName = "NoMatch";
  let Predicates = [pred];
}

let hasSideEffects = 0 in {
def SETR_SH    : ShortRdImm<SETR, "setr\t$rd, $imm">;
def SETRU_SH   : ShortRdImm<SETRU, "setru\t$rd, $imm", uimm8>;
def ANDI_SH    : ShortRdRsImm<ANDI, "andi\t$rd, $rs, $imm">;
def ORI_SH     : ShortRdRsImm<ORI, "ori\t$rd, $rs, $imm">;
def XORI_SH    : ShortRdRsImm<XORI, "xori\t$rd, $rs, $imm">;
def ANDV_SH    : ShortRdRsImm<ANDV, "andv\t$rd, $rs, $imm", uimm8>;
def ORV_SH     : ShortRdRsImm<ORV, "orv\t$rd, $rs, $imm", uimm8>;
def XORV_SH    : ShortRdRsImm<XORV, "xorv\t$rd, $rs, $imm", uimm8>;
let Defs = [FLAGS] in {
def ADDV_SH    : ShortRdRsImm<ADDV, "addv\t$rd, $rs, $imm", uimm8>;
def MINUSV_SH  : ShortRdRsImm<MINUSV, "minusv\t$rd, $rs, $imm", uimm8>;
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
// A1 short PC-relative branches (1 word, simm18 words). Produced by their
// CompressPats (below) for any branch to a symbolic target and relaxed back to
// the 2-word PC-relative form by the asm backend when out of range / not local.
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
// The short jump of a tail call (TAILREL's CompressPat): JMP_SH's encoding,
// but its target is a CallTarget18 like CALL_SH's, so a callee the assembler
// cannot resolve keeps it short with an R_KLAUSSCPU_CALL18 relocation (lld
// reaches a far callee through its jmprel thunk) instead of relaxing to the
// 2-word TAILREL.
let isBranch = 1, isTerminator = 1, isBarrier = 1, isAsmParserOnly = 1 in
def TAIL_SH : ShortBranch<JMP, "tail\t$target", CallTarget18, HasShortCalls>;
// A1 short PC-relative call (0x63000000): pushes PC+4, the address of the
// next instruction (pipeline_core.sv U_CALL: exo_wdata = ex_pc_next).
// Produced by its CompressPats for any call to a symbol; relaxed to CALLREL
// only when the assembler resolves the target and it is out of range.
let isCall = 1, Uses = [SP],
    Defs = [FLAGS] in
def CALL_SH : ShortBranch<CALL_I, "callrel\t$target", CallTarget18,
                          HasShortCalls>;

let mayStore = 1 in {
def STIDX8_SH    : ShortStore<STIDX8, 0, "stidx8\t$rs_data, $base, $offset">;
def STIDX16_SH   : ShortStore<STIDX16, 1, "stidx16\t$rs_data, $base, $offset">;
def STIDX32_SH   : ShortStore<STIDX32, 2, "stidx32\t$rs_data, $base, $offset">;
def STIDX64_SH   : ShortStore<STIDX64, 3, "stidx64\t$rs_data, $base, $offset">;
def STIDX64U_SH  : ShortStore<STIDX64U, 3, "stidx64u\t$rs_data, $base, $offset">;
}
}

// Long -> short compression (-gen-compress-inst-emitter; KlaussCPU::compress).
// The immediate forms are compress-only.  A short branch expands back to the
// 2-word PC-relative form (to the absolute one for JMPO/JMPNO, which have
// none): that is how the asm backend relaxes it (KlaussCPU::uncompress), and
// what makes it relaxable at all.
let isCompressOnly = true in {
let Predicates = [HasShortForms] in {
def : CompressPat<(SETR GPR:$rd, simm8_lsl0:$imm),
                  (SETR_SH GPR:$rd, simm8_lsl0:$imm)>;
// 128..255: the zero-extending short MOV loads the same value.
def : CompressPat<(SETR GPR:$rd, uimm8:$imm),
                  (SETRU_SH GPR:$rd, uimm8:$imm)>;
def : CompressPat<(SETRU GPR:$rd, uimm8:$imm),
                  (SETRU_SH GPR:$rd, uimm8:$imm)>;
foreach op = ["ADDI", "ANDI", "ORI", "XORI"] in
def : CompressPat<(!cast<KCInst64>(op) GPR:$rd, GPR:$rs, simm8_lsl0:$imm),
                  (!cast<KCInst32>(op # "_SH") GPR:$rd, GPR:$rs,
                                               simm8_lsl0:$imm)>;
def : CompressPat<(CMPRV_I GPR:$rs, simm8_lsl0:$imm),
                  (CMPRV_SH GPR:$rs, simm8_lsl0:$imm)>;
foreach op = ["ADDV", "MINUSV", "ANDV", "ORV", "XORV"] in
def : CompressPat<(!cast<KCInst64>(op) GPR:$rd, GPR:$rs, uimm8:$imm),
                  (!cast<KCInst32>(op # "_SH") GPR:$rd, GPR:$rs, uimm8:$imm)>;
// 128..255 (after the simm8 patterns above): the zero-extending short twin
// adds / masks with the same value and writes the same flags.
foreach op = [["ADDI", "ADDV"], ["ANDI", "ANDV"], ["ORI", "ORV"],
              ["XORI", "XORV"]] in
def : CompressPat<(!cast<KCInst64>(op[0]) GPR:$rd, GPR:$rs, uimm8:$imm),
                  (!cast<KCInst32>(op[1] # "_SH") GPR:$rd, GPR:$rs,
                                                  uimm8:$imm)>;

foreach ld = [["LDIDX8", "0"], ["LDIDX8_S", "0"], ["LDIDX16", "1"],
              ["LDIDX16_S", "1"], ["LDIDX32", "2"], ["LDIDX32_S", "2"],
              ["LDIDX64", "3"], ["LDIDX64U", "3"]] in {
  defvar off = !cast<Operand>("simm8_lsl" # ld[1]);
  def : CompressPat<(!cast<KCInst64>(ld[0]) GPR:$rd, GPR:$base, off:$offset),
                    (!cast<KCInst32>(ld[0] # "_SH") GPR:$rd, GPR:$base,
                                                    off:$offset)>;
}
foreach st = [["STIDX8", "0"], ["STIDX16", "1"], ["STIDX32", "2"],
              ["STIDX64", "3"], ["STIDX64U", "3"]] in {
  defvar off = !cast<Operand>("simm8_lsl" # st[1]);
  def : CompressPat<(!cast<KCInst64>(st[0]) GPR:$rs_data, GPR:$base,
                                            off:$offset),
                    (!cast<KCInst32>(st[0] # "_SH") GPR:$rs_data, GPR:$base,
                                                    off:$offset)>;
}
}
}

let Predicates = [HasShortBranches] in
foreach c = ["", "Z", "NZ", "C", "NC", "O", "NO", "S", "NS", "LT", "GE", "LE",
             "GT", "ULT", "UGE", "ULE", "UGT", "E", "NE"] in {
  defvar short = !cast<KCInst32>("JMP" # c # "_SH");
  defvar hasRel = !exists<KCInst64>("JMP" # c # "REL");
  let isCompressOnly = hasRel in
  def : CompressPat<(!cast<KCInst64>("JMP" # c) PCRelTarget18:$target),
                    (short PCRelTarget18:$target)>;
  if hasRel then
  def : CompressPat<(!cast<KCInst64>("JMP" # c # "REL") PCRelTarget18:$target),
                    (short PCRelTarget18:$target)>;
}

// A call to a symbol, absolute (non-PIC) or PC-relative, is the short call;
// it expands to CALLREL.  A tail call's jump likewise (TAIL_SH / TAILREL).
let Predicates = [HasShortCalls] in {
let isCompressOnly = true in
def : CompressPat<(CALL_I CallTarget18:$target), (CALL_SH CallTarget18:$target)>;
def : CompressPat<(CALLREL CallTarget18:$target),
                  (CALL_SH CallTarget18:$target)>;
def : CompressPat<(TAILREL CallTarget18:$target),
                  (TAIL_SH CallTarget18:$target)>;
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
// under-aligned i64 access (allowsMisalignedMemoryAccesses() is false for
// i64).
def : Pat<(i64 (load (add (i64 GPR:$base), (i64 GPR:$idx)))),
          (LDIDX64R GPR:$base, GPR:$idx)>;
def : Pat<(store (i64 GPR:$val), (add (i64 GPR:$base), (i64 GPR:$idx))),
          (STIDX64R GPR:$val, GPR:$base, GPR:$idx)>;

// The same for the sub-word accesses (`s[i]`, `a[i]` of char/short/int, the
// jump-table entry load): 1-word LDIDX{8,16,32}[_S]R / STIDX{8,16,32}R
// instead of ADDR + MEMGETn / MEMSETn.  The 16/32-bit forms force-align
// their EA, as LDIDX16/32 and STIDX16/32 do, so they take only naturally
// aligned accesses (allowsMisalignedMemoryAccesses allows none but an
// under-aligned 32-bit load, which the _a4 fragments exclude).
multiclass RegRegLoadPat<PatFrag ld, Instruction Inst> {
  def : Pat<(i64 (ld (add (i64 GPR:$base), (i64 GPR:$idx)))),
            (Inst GPR:$base, GPR:$idx)>;
}
defm : RegRegLoadPat<zextloadi8,  LDIDX8R>;
defm : RegRegLoadPat<extloadi8,   LDIDX8R>;
defm : RegRegLoadPat<sextloadi8,  LDIDX8_SR>;
defm : RegRegLoadPat<zextloadi16, LDIDX16R>;
defm : RegRegLoadPat<extloadi16,  LDIDX16R>;
defm : RegRegLoadPat<sextloadi16, LDIDX16_SR>;
defm : RegRegLoadPat<zextloadi32_a4, LDIDX32R>;
defm : RegRegLoadPat<extloadi32_a4,  LDIDX32R>;
defm : RegRegLoadPat<sextloadi32_a4, LDIDX32_SR>;
def : Pat<(truncstorei8  (i64 GPR:$val), (add (i64 GPR:$base), (i64 GPR:$idx))),
          (STIDX8R  GPR:$val, GPR:$base, GPR:$idx)>;
def : Pat<(truncstorei16 (i64 GPR:$val), (add (i64 GPR:$base), (i64 GPR:$idx))),
          (STIDX16R GPR:$val, GPR:$base, GPR:$idx)>;
def : Pat<(truncstorei32 (i64 GPR:$val), (add (i64 GPR:$base), (i64 GPR:$idx))),
          (STIDX32R GPR:$val, GPR:$base, GPR:$idx)>;

//===----------------------------------------------------------------------===//
// Atomic loads and stores ("forced atomics", KlaussCPUISelLowering.cpp)
//===----------------------------------------------------------------------===//
// A naturally aligned atomic load or store of 1/2/4/8 bytes is the plain
// access, whatever its ordering: the in-order core makes every access visible
// in program order.  The same addressing modes as the plain accesses: [r]
// (offset 0), r + simm32, a frame slot (AddrFIMem) and r + r.  A sub-word
// atomic load is zero-extending by default (getExtendForAtomicOps); the
// combiner may turn it into a sign-extending one.  Every form is naturally
// aligned (AtomicExpand makes a misaligned atomic a libcall), so the forced
// EA alignment of the 16/32-bit forms does not matter.
multiclass AtomicLoadPat<PatFrags ld, Instruction Inst, Instruction InstRR> {
  def : Pat<(i64 (ld (i64 GPR:$base))), (Inst GPR:$base, 0)>;
  def : Pat<(i64 (ld (add (i64 GPR:$base), simm32_imm:$off))),
            (Inst GPR:$base, simm32_imm:$off)>;
  let AddedComplexity = 10 in
  def : Pat<(i64 (ld (AddrFIMem (i64 GPR:$base), simm32_imm:$off))),
            (Inst GPR:$base, simm32_imm:$off)>;
  def : Pat<(i64 (ld (add (i64 GPR:$base), (i64 GPR:$idx)))),
            (InstRR GPR:$base, GPR:$idx)>;
}
multiclass AtomicStorePat<PatFrag st, Instruction Inst, Instruction InstRR> {
  def : Pat<(st (i64 GPR:$val), (i64 GPR:$base)),
            (Inst GPR:$val, GPR:$base, 0)>;
  def : Pat<(st (i64 GPR:$val), (add (i64 GPR:$base), simm32_imm:$off)),
            (Inst GPR:$val, GPR:$base, simm32_imm:$off)>;
  let AddedComplexity = 10 in
  def : Pat<(st (i64 GPR:$val), (AddrFIMem (i64 GPR:$base), simm32_imm:$off)),
            (Inst GPR:$val, GPR:$base, simm32_imm:$off)>;
  def : Pat<(st (i64 GPR:$val), (add (i64 GPR:$base), (i64 GPR:$idx))),
            (InstRR GPR:$val, GPR:$base, GPR:$idx)>;
}
defm : AtomicLoadPat<atomic_load_nonext_64, LDIDX64,   LDIDX64R>;
defm : AtomicLoadPat<atomic_load_azext_32,  LDIDX32,   LDIDX32R>;
defm : AtomicLoadPat<atomic_load_sext_32,   LDIDX32_S, LDIDX32_SR>;
defm : AtomicLoadPat<atomic_load_azext_16,  LDIDX16,   LDIDX16R>;
defm : AtomicLoadPat<atomic_load_sext_16,   LDIDX16_S, LDIDX16_SR>;
defm : AtomicLoadPat<atomic_load_azext_8,   LDIDX8,    LDIDX8R>;
defm : AtomicLoadPat<atomic_load_sext_8,    LDIDX8_S,  LDIDX8_SR>;
defm : AtomicStorePat<atomic_store_64, STIDX64, STIDX64R>;
defm : AtomicStorePat<atomic_store_32, STIDX32, STIDX32R>;
defm : AtomicStorePat<atomic_store_16, STIDX16, STIDX16R>;
defm : AtomicStorePat<atomic_store_8,  STIDX8,  STIDX8R>;
// And at a constant address, the absolute forms.
let AddedComplexity = 10 in {
defm : AbsLoadPat<atomic_load_nonext_64, AbsAddr64, MEMREADR>;
defm : AbsLoadPat<atomic_load_azext_32,  AbsAddr32, LDABS32>;
defm : AbsLoadPat<atomic_load_sext_32,   AbsAddr32, LDABS32_S>;
defm : AbsLoadPat<atomic_load_azext_16,  AbsAddr16, LDABS16>;
defm : AbsLoadPat<atomic_load_sext_16,   AbsAddr16, LDABS16_S>;
defm : AbsLoadPat<atomic_load_azext_8,   AbsAddr8,  LDABS8>;
defm : AbsLoadPat<atomic_load_sext_8,    AbsAddr8,  LDABS8_S>;
defm : AbsStorePat<atomic_store_64, AbsAddr64, MEMSETR>;
defm : AbsStorePat<atomic_store_32, AbsAddr32, STABS32>;
defm : AbsStorePat<atomic_store_16, AbsAddr16, STABS16>;
defm : AbsStorePat<atomic_store_8,  AbsAddr8,  STABS8>;
}
