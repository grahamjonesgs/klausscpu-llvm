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
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Casting.h"
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
  StringRef      Tok;
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

  AsmLexer    &getLexer() { return Parser.getLexer(); }

  // TableGen-generated: ComputeAvailableFeatures, convertToMCInst,
  //                     convertToMapAndConstraints, MatchInstructionImpl.
#define GET_ASSEMBLER_HEADER
#include "KlaussCPUGenAsmMatcher.inc"

  MCRegister matchRegisterByName(StringRef Name);
  ParseStatus parseOperand(OperandVector &Operands, StringRef Mnemonic);

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
                                              StringRef /*Mnemonic*/) {
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

bool KlaussCPUAsmParser::parseInstruction(ParseInstructionInfo & /*Info*/,
                                           StringRef Name, SMLoc NameLoc,
                                           OperandVector &Operands) {
  Operands.push_back(KlaussCPUOperand::createToken(Name, NameLoc));

  if (getLexer().is(AsmToken::EndOfStatement) ||
      getLexer().is(AsmToken::Eof))
    return false;

  while (true) {
    if (parseOperand(Operands, Name).isFailure())
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
