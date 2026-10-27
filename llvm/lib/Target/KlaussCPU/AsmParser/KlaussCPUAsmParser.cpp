//===-- KlaussCPUAsmParser.cpp - KlaussCPU Assembly Parser ----------------===//
//
// Parses KlaussCPU assembly text into MCInst objects.  Used by llc when
// processing inline asm blocks and by llvm-mc for standalone assembly.
//
// Register syntax:  r0–r15  (or single-letter aliases a–p, case-insensitive)
// Immediates:       integer literals or label expressions
// Instruction fmt:  mnemonic [operand [, operand]*]
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/KlaussCPUMCTargetDesc.h"
#include "TargetInfo/KlaussCPUTargetInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCParser/AsmLexer.h"
#include "llvm/MC/MCParser/MCAsmParser.h"
#include "llvm/MC/MCParser/MCParsedAsmOperand.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"

// Register and instruction enums — referenced by KlaussCPUGenAsmMatcher.inc.
#define GET_REGINFO_ENUM
#include "KlaussCPUGenRegisterInfo.inc"

#define GET_INSTRINFO_ENUM
#include "KlaussCPUGenInstrInfo.inc"

using namespace llvm;

namespace {

//===----------------------------------------------------------------------===//
// KlaussCPUOperand — parsed assembly operand
//===----------------------------------------------------------------------===//

class KlaussCPUOperand : public MCParsedAsmOperand {
public:
  enum KindTy { Token, Register, Immediate };

  // Public constructor so std::make_unique can call it.
  KlaussCPUOperand(KindTy K, SMLoc S, SMLoc E)
      : Kind(K), StartLoc(S), EndLoc(E) {}

  // ── Factory methods ─────────────────────────────────────────────────────

  static std::unique_ptr<KlaussCPUOperand>
  createToken(StringRef Str, SMLoc Loc) {
    auto Op = std::make_unique<KlaussCPUOperand>(Token, Loc, Loc);
    Op->Tok = Str;
    return Op;
  }

  static std::unique_ptr<KlaussCPUOperand>
  createReg(MCRegister Reg, SMLoc S, SMLoc E) {
    auto Op = std::make_unique<KlaussCPUOperand>(Register, S, E);
    Op->RegNum = Reg;
    return Op;
  }

  static std::unique_ptr<KlaussCPUOperand>
  createImm(const MCExpr *Val, SMLoc S, SMLoc E) {
    auto Op = std::make_unique<KlaussCPUOperand>(Immediate, S, E);
    Op->ImmVal = Val;
    return Op;
  }

  // ── MCParsedAsmOperand interface ─────────────────────────────────────────

  bool isToken() const override { return Kind == Token; }
  bool isReg()   const override { return Kind == Register; }
  bool isMem()   const override { return false; }
  bool isImm()   const override { return Kind == Immediate; }

  bool isConstantImm(int64_t &Val) const {
    if (!isImm())
      return false;
    const auto *CE = dyn_cast<MCConstantExpr>(ImmVal);
    if (!CE)
      return false;
    Val = CE->getValue();
    return true;
  }

  // Fused-branch condition PRED | INV << 3; PRED 5..7 trap on the CPU.
  bool isFBRCond() const {
    int64_t Val;
    return isConstantImm(Val) && isUInt<4>(Val) && (Val & 7) <= 4;
  }

  bool isSImm4() const {
    int64_t Val;
    return isConstantImm(Val) && isInt<4>(Val);
  }

  MCRegister getReg() const override {
    assert(isReg() && "not a register operand");
    return RegNum;
  }

  // getToken() is called by the generated MatchInstructionImpl to extract the
  // mnemonic from Operands[0].  Not virtual in MCParsedAsmOperand.
  StringRef getToken() const {
    assert(isToken() && "not a token operand");
    return Tok;
  }

  const MCExpr *getImm() const {
    assert(isImm() && "not an immediate operand");
    return ImmVal;
  }

  SMLoc getStartLoc() const override { return StartLoc; }
  SMLoc getEndLoc()   const override { return EndLoc; }

  // ── Render methods (called by MatchInstructionImpl) ──────────────────────

  void addRegOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && isReg());
    Inst.addOperand(MCOperand::createReg(getReg()));
  }

  void addImmOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && isImm());
    const MCExpr *Expr = getImm();
    if (const auto *CE = dyn_cast<MCConstantExpr>(Expr))
      Inst.addOperand(MCOperand::createImm(CE->getValue()));
    else
      Inst.addOperand(MCOperand::createExpr(Expr));
  }

  // MCExpr::print() is private in LLVM 23; just note the kind for debugging.
  void print(raw_ostream &OS, const MCAsmInfo & /*MAI*/) const override {
    switch (Kind) {
    case Token:    OS << "Tok:"  << Tok;    break;
    case Register: OS << "Reg:"  << RegNum; break;
    case Immediate:OS << "Imm";             break;
    }
  }

private:
  KindTy Kind;
  SMLoc  StartLoc, EndLoc;
  // Owns its storage: the mnemonic token is lower-cased from the source text
  // (which may be UPPERCASE per opcode_select.vh), so it cannot alias the
  // original buffer.
  std::string    Tok;
  MCRegister     RegNum;
  const MCExpr  *ImmVal = nullptr;
};

//===----------------------------------------------------------------------===//
// KlaussCPUAsmParser
//===----------------------------------------------------------------------===//

class KlaussCPUAsmParser : public MCTargetAsmParser {
  // Store a reference to the parser for Lex() / parseExpression().
  // Do NOT store a separate STI member — the base class has
  //   const MCSubtargetInfo *STI;
  // and the generated MatchInstructionImpl uses "*STI".  A shadowing member
  // of reference type would make that dereference ill-formed.
  MCAsmParser &Parser;

  // Set while parsing a PC-relative target operand; PCRelAbsSym is the first
  // absolute .set/.equ symbol it names (see parsePCRelTarget).
  bool InPCRelTarget = false;
  MCSymbol *PCRelAbsSym = nullptr;
  int64_t PCRelAbsVal = 0;

  AsmLexer    &getLexer() { return Parser.getLexer(); }

  // TableGen-generated: ComputeAvailableFeatures, convertToMCInst,
  //                     convertToMapAndConstraints, MatchInstructionImpl.
#define GET_ASSEMBLER_HEADER
#include "KlaussCPUGenAsmMatcher.inc"

  MCRegister matchRegisterByName(StringRef Name);
  ParseStatus parseOperand(OperandVector &Operands, StringRef Mnemonic);
  ParseStatus parsePCRelTarget(OperandVector &Operands);

public:
  KlaussCPUAsmParser(const MCSubtargetInfo &STI, MCAsmParser &Parser,
                      const MCInstrInfo &MII,
                      const MCTargetOptions &Options)
      : MCTargetAsmParser(Options, STI, MII), Parser(Parser) {
    setAvailableFeatures(ComputeAvailableFeatures(STI.getFeatureBits()));
  }

  bool parseRegister(MCRegister &Reg, SMLoc &StartLoc,
                     SMLoc &EndLoc) override;
  ParseStatus tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                SMLoc &EndLoc) override;
  bool parseInstruction(ParseInstructionInfo &Info, StringRef Name,
                         SMLoc NameLoc, OperandVector &Operands) override;
  bool ParseDirective(AsmToken DirectiveID) override { return true; }
  bool parsePrimaryExpr(const MCExpr *&Res, SMLoc &EndLoc) override;
  bool matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                                OperandVector &Operands, MCStreamer &Out,
                                uint64_t &ErrorInfo,
                                bool MatchingInlineAsm) override;
};

} // end anonymous namespace

// Pull in register matcher (MatchRegisterName) and full instruction matcher.
#define GET_REGISTER_MATCHER
#define GET_MATCHER_IMPLEMENTATION
#include "KlaussCPUGenAsmMatcher.inc"

//===----------------------------------------------------------------------===//
// Register parsing
//===----------------------------------------------------------------------===//

// The generated MatchRegisterName handles r0–r15/sp/ra.
// Single-letter aliases (a–p) are handled with a manual table here.
static MCRegister matchRegisterAlias(StringRef Name) {
  if (Name.size() != 1)
    return MCRegister();
  // Aliases a(=r0)…p(=r15), lowercase only.
  static const struct { char alias; MCRegister reg; } Tbl[] = {
    {'a', KlaussCPU::R0},  {'b', KlaussCPU::R1},
    {'c', KlaussCPU::R2},  {'d', KlaussCPU::R3},
    {'e', KlaussCPU::R4},  {'f', KlaussCPU::R5},
    {'g', KlaussCPU::R6},  {'h', KlaussCPU::R7},
    {'i', KlaussCPU::R8},  {'j', KlaussCPU::R9},
    {'k', KlaussCPU::R10}, {'l', KlaussCPU::R11},
    {'m', KlaussCPU::R12}, {'n', KlaussCPU::R13},
    {'o', KlaussCPU::R14}, {'p', KlaussCPU::R15},
  };
  char c = Name[0];
  for (const auto &E : Tbl)
    if (E.alias == c)
      return E.reg;
  return MCRegister();
}

MCRegister KlaussCPUAsmParser::matchRegisterByName(StringRef Name) {
  std::string Lower = Name.lower();
  if (MCRegister R = MatchRegisterName(Lower))
    return R;
  return matchRegisterAlias(Lower);
}

bool KlaussCPUAsmParser::parseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                        SMLoc &EndLoc) {
  ParseStatus S = tryParseRegister(Reg, StartLoc, EndLoc);
  if (S.isFailure())
    return Error(StartLoc, "invalid register name");
  return !S.isSuccess();
}

ParseStatus KlaussCPUAsmParser::tryParseRegister(MCRegister &Reg,
                                                   SMLoc &StartLoc,
                                                   SMLoc &EndLoc) {
  const AsmToken &Tok = getLexer().getTok();
  if (Tok.isNot(AsmToken::Identifier))
    return ParseStatus::NoMatch;

  Reg = matchRegisterByName(Tok.getString());
  if (!Reg)
    return ParseStatus::NoMatch;

  StartLoc = Tok.getLoc();
  EndLoc   = Tok.getEndLoc();
  Parser.Lex();
  return ParseStatus::Success;
}

//===----------------------------------------------------------------------===//
// Operand and instruction parsing
//===----------------------------------------------------------------------===//

ParseStatus KlaussCPUAsmParser::parseOperand(OperandVector &Operands,
                                              StringRef Mnemonic) {
  // Operand classes with a ParserMethod in the .td (PC-relative targets).
  ParseStatus Res = MatchOperandParserImpl(Operands, Mnemonic);
  if (!Res.isNoMatch())
    return Res;

  SMLoc S = getLexer().getLoc();

  // Try register first.
  if (getLexer().is(AsmToken::Identifier)) {
    MCRegister Reg = matchRegisterByName(getLexer().getTok().getString());
    if (Reg) {
      SMLoc E = getLexer().getTok().getEndLoc();
      Operands.push_back(KlaussCPUOperand::createReg(Reg, S, E));
      Parser.Lex();
      return ParseStatus::Success;
    }
  }

  // Otherwise parse as an expression (literal, label, or arithmetic).
  const MCExpr *Expr;
  SMLoc E;
  if (Parser.parseExpression(Expr, E))
    return ParseStatus::Failure;

  Operands.push_back(KlaussCPUOperand::createImm(Expr, S, E));
  return ParseStatus::Success;
}

// A PC-relative target (PCRelTargetAsmOperand): a bare number is the
// displacement from the instruction, as the disassembler prints it, and an
// operand that names a symbol is the target address, which the PC-relative
// fixup turns into a displacement or a relocation.  The generic parser folds
// an absolute .set/.equ symbol into a plain number, so `callrel ROMFN` would
// read as PC + ROMFN.  parsePrimaryExpr notes such a symbol, and a folded
// result is rebuilt as SYM + (value - SYM): the same target as when the .set
// follows the use, in every spelling (`(SYM)`, `4+SYM`, `-SYM`).
ParseStatus KlaussCPUAsmParser::parsePCRelTarget(OperandVector &Operands) {
  if (getLexer().is(AsmToken::Identifier) &&
      matchRegisterByName(getLexer().getTok().getString()))
    return ParseStatus::NoMatch;

  SMLoc S = getLexer().getLoc();
  SaveAndRestore InTarget(InPCRelTarget, true);
  SaveAndRestore<MCSymbol *> AbsSym(PCRelAbsSym, nullptr);
  const MCExpr *Expr;
  SMLoc E;
  if (Parser.parseExpression(Expr, E))
    return ParseStatus::Failure;

  if (const auto *CE = dyn_cast<MCConstantExpr>(Expr); CE && PCRelAbsSym) {
    Expr = MCSymbolRefExpr::create(PCRelAbsSym, getContext(), S);
    if (CE->getValue() != PCRelAbsVal)
      Expr = MCBinaryExpr::createAdd(
          Expr, MCConstantExpr::create(CE->getValue() - PCRelAbsVal,
                                       getContext()),
          getContext(), S);
  }

  Operands.push_back(KlaussCPUOperand::createImm(Expr, S, E));
  return ParseStatus::Success;
}

bool KlaussCPUAsmParser::parsePrimaryExpr(const MCExpr *&Res, SMLoc &EndLoc) {
  if (!InPCRelTarget)
    return MCTargetAsmParser::parsePrimaryExpr(Res, EndLoc);

  // The generic parser parses a unary operator's operand itself, without
  // coming back here, so handle those to see the symbol in `-SYM`.
  SMLoc S = getLexer().getLoc();
  MCUnaryExpr::Opcode Op;
  switch (getLexer().getKind()) {
  case AsmToken::Minus:
    Op = MCUnaryExpr::Minus;
    break;
  case AsmToken::Plus:
    Op = MCUnaryExpr::Plus;
    break;
  case AsmToken::Tilde:
    Op = MCUnaryExpr::Not;
    break;
  case AsmToken::Exclaim:
    Op = MCUnaryExpr::LNot;
    break;
  case AsmToken::Identifier:
    if (!PCRelAbsSym)
      if (MCSymbol *Sym = getContext().lookupSymbol(
              getLexer().getTok().getIdentifier()))
        if (Sym->isVariable())
          if (const auto *CE =
                  dyn_cast<MCConstantExpr>(Sym->getVariableValue())) {
            PCRelAbsSym = Sym;
            PCRelAbsVal = CE->getValue();
          }
    [[fallthrough]];
  default:
    return MCTargetAsmParser::parsePrimaryExpr(Res, EndLoc);
  }
  Parser.Lex();
  if (parsePrimaryExpr(Res, EndLoc))
    return true;
  Res = MCUnaryExpr::create(Op, Res, getContext(), S);
  return false;
}

bool KlaussCPUAsmParser::parseInstruction(ParseInstructionInfo & /*Info*/,
                                           StringRef Name, SMLoc NameLoc,
                                           OperandVector &Operands) {
  // The .td AsmStrings (and therefore the match table) are lower-case, while
  // opcode_select.vh spells mnemonics in UPPERCASE.  Lower the mnemonic here so
  // assembly written in either case (ADDR / addr / AddR) matches.  Registers
  // are already case-folded in matchRegisterByName().
  Operands.push_back(KlaussCPUOperand::createToken(Name.lower(), NameLoc));
  // The match tables (and so MatchOperandParserImpl) use the lower-case name.
  StringRef Mnemonic =
      static_cast<KlaussCPUOperand &>(*Operands[0]).getToken();

  if (getLexer().is(AsmToken::EndOfStatement) ||
      getLexer().is(AsmToken::Eof))
    return false;

  while (true) {
    if (parseOperand(Operands, Mnemonic).isFailure())
      return Error(getLexer().getLoc(), "failed to parse operand");

    if (getLexer().is(AsmToken::EndOfStatement) ||
        getLexer().is(AsmToken::Eof))
      break;

    if (!getLexer().is(AsmToken::Comma))
      return Error(getLexer().getLoc(), "expected ',' or end of statement");

    Parser.Lex();
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Instruction matching and emission
//===----------------------------------------------------------------------===//

bool KlaussCPUAsmParser::matchAndEmitInstruction(SMLoc IDLoc,
                                                   unsigned & /*Opcode*/,
                                                   OperandVector &Operands,
                                                   MCStreamer &Out,
                                                   uint64_t &ErrorInfo,
                                                   bool MatchingInlineAsm) {
  MCInst Inst;
  unsigned Result =
      MatchInstructionImpl(Operands, Inst, ErrorInfo, MatchingInlineAsm);

  switch (Result) {
  case Match_Success:
    Inst.setLoc(IDLoc);
    compressKlaussCPUInst(Inst); // ISA v3 short 1-word forms
    Out.emitInstruction(Inst, getSTI());
    return false;

  case Match_MissingFeature:
    return Error(IDLoc, "instruction requires an unsupported feature");

  case Match_MnemonicFail:
    return Error(IDLoc, "unrecognised instruction mnemonic");

  case Match_InvalidOperand: {
    SMLoc ErrLoc = IDLoc;
    if (ErrorInfo != ~0ULL) {
      if (ErrorInfo >= Operands.size())
        return Error(IDLoc, "too few operands for instruction");
      ErrLoc = Operands[ErrorInfo]->getStartLoc();
    }
    return Error(ErrLoc, "invalid operand for instruction");
  }

  default:
    return true;
  }
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeKlaussCPUAsmParser() {
  RegisterMCAsmParser<KlaussCPUAsmParser> X(getTheKlaussCPUTarget());
}
