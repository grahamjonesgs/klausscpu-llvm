//===-- KlaussCPUAsmParser.cpp - KlaussCPU Assembly Parser ----------------===//
//
// Parses KlaussCPU assembly text into MCInst objects.  Used by llc when
// processing inline asm blocks and by llvm-mc for standalone assembly.
//
// Register syntax:  r0–r15  (or single-letter aliases a–p, case-insensitive)
// Immediates:       integer literals or label expressions, range-checked by
//                   the operand classes in KlaussCPUInstrInfo.td
// Instruction fmt:  mnemonic [operand [, operand]*]
//
// There is no register prefix, so a symbol can be spelled like a register
// (`call b`, `leapc r12, sp`).  A lone register name is parsed as a register
// operand that also carries the expression for the symbol of that name; it
// matches a register class as the register and an immediate class as the
// symbol, so the instruction's operand list decides.  As with RISC-V's
// `call a0`, a register name in a symbol-capable slot is therefore never an
// assembler error: `setr r1, r2` or `call r1` assembles to a relocation
// against a symbol `r2` / `r1`, and a typo shows up only as an undefined
// symbol at link time.
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

  // SymExpr: the same identifier read as an expression (see the file
  // header), or null.
  static std::unique_ptr<KlaussCPUOperand>
  createReg(MCRegister Reg, SMLoc S, SMLoc E,
            const MCExpr *SymExpr = nullptr) {
    auto Op = std::make_unique<KlaussCPUOperand>(Register, S, E);
    Op->RegNum = Reg;
    Op->ImmVal = SymExpr;
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

  // The operand as an immediate: an expression, or a register name read
  // as a symbol.  Null for a token or a register with no symbol reading.
  const MCExpr *getImmExpr() const {
    return Kind == Token ? nullptr : ImmVal;
  }

  bool isConstantImm(int64_t &Val) const {
    const auto *CE = dyn_cast_or_null<MCConstantExpr>(getImmExpr());
    if (!CE)
      return false;
    Val = CE->getValue();
    return true;
  }

  // Immediate operand classes (KCImmAsmOperand): a constant in [Lo, Hi] that
  // is a multiple of Scale; with AllowSym also a symbolic expression, which
  // the operand's EncoderMethod turns into a fixup.
  template <int64_t Lo, int64_t Hi, int64_t Scale, bool AllowSym>
  bool isImmRange() const {
    if (!getImmExpr())
      return false;
    int64_t Val;
    if (!isConstantImm(Val))
      return AllowSym;
    return Val >= Lo && Val <= Hi && Val % Scale == 0;
  }

  // The imm32 of ADDI and the indexed loads / stores (SImm32TPRelAsmOperand):
  // a signed 32-bit constant, or an expression with `%tprel` (TLS local exec).
  bool isSImm32TPRel() const {
    const MCExpr *Expr = getImmExpr();
    if (!Expr)
      return false;
    int64_t Val;
    if (isConstantImm(Val))
      return isInt<32>(Val);
    return KlaussCPU::getExprSpecifier(Expr) == KlaussCPU::S_TPREL;
  }
  // Fused-branch condition PRED | INV << 3 (PRED 5..7 are the W compares).
  bool isFBRCond() const {
    int64_t Val;
    return isConstantImm(Val) && isUInt<4>(Val);
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

  SMLoc getStartLoc() const override { return StartLoc; }
  SMLoc getEndLoc()   const override { return EndLoc; }

  // ── Render methods (called by MatchInstructionImpl) ──────────────────────

  void addRegOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && isReg());
    Inst.addOperand(MCOperand::createReg(getReg()));
  }

  void addImmOperands(MCInst &Inst, unsigned N) const {
    const MCExpr *Expr = getImmExpr();
    assert(N == 1 && Expr && "not an immediate operand");
    if (const auto *CE = dyn_cast<MCConstantExpr>(Expr))
      Inst.addOperand(MCOperand::createImm(CE->getValue()));
    else
      Inst.addOperand(MCOperand::createExpr(Expr));
  }

  // A raw 32-bit pattern (SETR64 halves, SETR's imm32): a constant above
  // INT32_MAX is the same pattern as its sign-extension.  Normalise to the
  // signed form, which is what the disassembler and the InstPrinter print,
  // so -show-encoding and llvm-objdump agree.
  void addImm32Operands(MCInst &Inst, unsigned N) const {
    const MCExpr *Expr = getImmExpr();
    assert(N == 1 && Expr && "not an immediate operand");
    if (const auto *CE = dyn_cast<MCConstantExpr>(Expr))
      Inst.addOperand(MCOperand::createImm(
          isUInt<32>(CE->getValue()) ? SignExtend64<32>(CE->getValue())
                                     : CE->getValue()));
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
  // (which may be UPPERCASE, as the ISA map spells it), so it cannot alias
  // the original buffer.
  std::string    Tok;
  MCRegister     RegNum;
  // Immediate: the value.  Register: the symbol reading of its name, or null.
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
  // absolute .set/.equ symbol it names (see parsePCRelExpr).
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
  ParseStatus parseImmOrRegName(OperandVector &Operands, bool PCRel);
  bool parsePCRelExpr(const MCExpr *&Expr, SMLoc &E);
  ParseStatus parsePCRelTarget(OperandVector &Operands) {
    return parseImmOrRegName(Operands, /*PCRel=*/true);
  }
  ParseStatus parseSysReg(OperandVector &Operands);

public:
  enum KlaussCPUMatchResultTy : unsigned {
    Match_Dummy = FIRST_TARGET_MATCH_RESULT_TY,
#define GET_OPERAND_DIAGNOSTIC_TYPES
#include "KlaussCPUGenAsmMatcher.inc"
#undef GET_OPERAND_DIAGNOSTIC_TYPES
  };

  KlaussCPUAsmParser(const MCSubtargetInfo &STI, MCAsmParser &Parser,
                      const MCInstrInfo &MII,
                      const MCTargetOptions &Options)
      : MCTargetAsmParser(Options, STI, MII), Parser(Parser) {
    setAvailableFeatures(ComputeAvailableFeatures(STI.getFeatureBits()));
    // GNU as data sizes, as on RISC-V: a word is 4 bytes (32-bit
    // addresses), a half-word 2, a double-word 8.
    Parser.addAliasForDirective(".half", ".2byte");
    Parser.addAliasForDirective(".hword", ".2byte");
    Parser.addAliasForDirective(".word", ".4byte");
    Parser.addAliasForDirective(".dword", ".8byte");
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

// r0–r15, sp, ra, flags (MatchRegisterName) and the letter names a–p
// (AltNames, MatchRegisterAltName), in any case.
MCRegister KlaussCPUAsmParser::matchRegisterByName(StringRef Name) {
  std::string Lower = Name.lower();
  if (MCRegister R = MatchRegisterName(Lower))
    return R;
  return MatchRegisterAltName(Lower);
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
  return parseImmOrRegName(Operands, /*PCRel=*/false);
}

// An expression (literal, label, or arithmetic), or a register name.  A
// register name that is the whole operand also keeps its reading as a symbol
// (see the file header); one that starts a longer expression (`b+8`) is the
// symbol.
ParseStatus KlaussCPUAsmParser::parseImmOrRegName(OperandVector &Operands,
                                                  bool PCRel) {
  SMLoc S = getLexer().getLoc();
  MCRegister Reg;
  if (getLexer().is(AsmToken::Identifier)) {
    AsmToken Next = getLexer().peekTok();
    if (Next.is(AsmToken::Comma) || Next.is(AsmToken::EndOfStatement) ||
        Next.is(AsmToken::Eof))
      Reg = matchRegisterByName(getLexer().getTok().getString());
  }

  const MCExpr *Expr;
  SMLoc E;
  if (PCRel ? parsePCRelExpr(Expr, E) : Parser.parseExpression(Expr, E))
    return ParseStatus::Failure;

  if (Reg)
    Operands.push_back(KlaussCPUOperand::createReg(Reg, S, E, Expr));
  else
    Operands.push_back(KlaussCPUOperand::createImm(Expr, S, E));
  return ParseStatus::Success;
}

// An MFSR / MTSR system register (SysRegAsmOperand): a name from
// KlaussCPU::getSysRegName, in any case, or an expression that the operand
// class requires to be a constant in [0, 255] (numbers without a name too).
ParseStatus KlaussCPUAsmParser::parseSysReg(OperandVector &Operands) {
  const AsmToken &Tok = getLexer().getTok();
  if (Tok.is(AsmToken::Identifier)) {
    AsmToken Next = getLexer().peekTok();
    if (Next.is(AsmToken::Comma) || Next.is(AsmToken::EndOfStatement) ||
        Next.is(AsmToken::Eof))
      if (std::optional<unsigned> Num =
              KlaussCPU::lookupSysReg(Tok.getIdentifier())) {
        SMLoc S = Tok.getLoc(), E = Tok.getEndLoc();
        Parser.Lex();
        Operands.push_back(KlaussCPUOperand::createImm(
            MCConstantExpr::create(*Num, getContext()), S, E));
        return ParseStatus::Success;
      }
  }
  return parseImmOrRegName(Operands, /*PCRel=*/false);
}

// A PC-relative target (PCRelAsmOperand): a bare number is the
// displacement from the instruction, as the disassembler prints it, and an
// operand that names a symbol is the target address, which the PC-relative
// fixup turns into a displacement or a relocation.  The generic parser folds
// an absolute .set/.equ symbol into a plain number, so `callrel ROMFN` would
// read as PC + ROMFN.  parsePrimaryExpr notes such a symbol, and a folded
// result is rebuilt as SYM + (value - SYM): the same target as when the .set
// follows the use, in every spelling (`(SYM)`, `4+SYM`, `-SYM`).
bool KlaussCPUAsmParser::parsePCRelExpr(const MCExpr *&Expr, SMLoc &E) {
  SMLoc S = getLexer().getLoc();
  SaveAndRestore InTarget(InPCRelTarget, true);
  SaveAndRestore<MCSymbol *> AbsSym(PCRelAbsSym, nullptr);
  if (Parser.parseExpression(Expr, E))
    return true;

  if (const auto *CE = dyn_cast<MCConstantExpr>(Expr); CE && PCRelAbsSym) {
    Expr = MCSymbolRefExpr::create(PCRelAbsSym, getContext(), S);
    if (CE->getValue() != PCRelAbsVal)
      Expr = MCBinaryExpr::createAdd(
          Expr, MCConstantExpr::create(CE->getValue() - PCRelAbsVal,
                                       getContext()),
          getContext(), S);
  }
  return false;
}

bool KlaussCPUAsmParser::parsePrimaryExpr(const MCExpr *&Res, SMLoc &EndLoc) {
  // A relocation specifier: `%tprel(expr)`, the TLS local-exec offset of a
  // symbol from the thread pointer (the operand classes say where it may
  // go), or a dynamic-linking one, `%gotpcrel(sym)`, `%plt(sym)`,
  // `%gottprel(sym)`, `%tlsgd(sym)`, which takes a bare symbol
  // (matchAndEmitInstruction checks the instruction).
  if (getLexer().is(AsmToken::Percent)) {
    SMLoc S = getLexer().getLoc();
    AsmToken Next = getLexer().peekTok();
    if (Next.isNot(AsmToken::Identifier))
      return TokError("expected relocation specifier after '%'");
    uint16_t Spec = KlaussCPU::parseSpecifierName(Next.getIdentifier());
    if (!Spec)
      return Error(Next.getLoc(), "unknown relocation specifier '%" +
                                      Next.getIdentifier() + "'");
    StringRef Name = KlaussCPU::getSpecifierName(Spec);
    Parser.Lex(); // '%'
    Parser.Lex(); // specifier
    if (getLexer().isNot(AsmToken::LParen))
      return TokError("expected '(' after %" + Name);
    Parser.Lex();
    SMLoc SubLoc = getLexer().getLoc();
    const MCExpr *Sub;
    {
      // The symbol is the operand of the specifier, not a branch target.
      SaveAndRestore NotTarget(InPCRelTarget, false);
      if (Parser.parseParenExpression(Sub, EndLoc))
        return true;
    }
    if (KlaussCPU::isPCRelSpecifier(Spec) && !isa<MCSymbolRefExpr>(Sub))
      return Error(SubLoc, "%" + Name +
                               " takes a symbol without an offset "
                               "(add the offset after the access)");
    Res = MCSpecifierExpr::create(Sub, Spec, getContext(), S);
    return false;
  }
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
  // the ISA map (ISA_ENCODING_V2_MAP.md) spells mnemonics in UPPERCASE.  Lower
  // the mnemonic here so assembly written in either case (ADDR / addr / AddR)
  // matches.  Registers are already case-folded in matchRegisterByName().
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
  case Match_Success: {
    Inst.setLoc(IDLoc);
    // A dynamic-linking specifier belongs to one operand of one instruction
    // (the word-1 immediate of a 2-word leapc, or callrel / jmprel / tail),
    // as the whole operand: `%plt(f)+4` would be a PLT entry plus 4.
    for (const MCOperand &MO : Inst) {
      if (!MO.isExpr())
        continue;
      uint16_t Spec = KlaussCPU::getExprSpecifier(MO.getExpr());
      if (!KlaussCPU::isPCRelSpecifier(Spec))
        continue;
      SMLoc Loc = IDLoc;
      for (const auto &Op : drop_begin(Operands))
        if (static_cast<const KlaussCPUOperand &>(*Op).getImmExpr() ==
            MO.getExpr())
          Loc = Op->getStartLoc();
      StringRef Name = KlaussCPU::getSpecifierName(Spec);
      if (!KlaussCPU::isSpecifierAllowed(Inst.getOpcode(), Spec))
        return Error(Loc, "%" + Name +
                              (Spec == KlaussCPU::S_PLT
                                   ? " is only allowed as the target of "
                                     "callrel, jmprel or tail"
                                   : " is only allowed as the offset of "
                                     "leapc"));
      if (!isa<MCSpecifierExpr>(MO.getExpr()))
        return Error(Loc, "%" + Name +
                              " takes a symbol without an offset "
                              "(add the offset after the access)");
    }
    // ISA v3 short 1-word forms.
    MCInst CInst;
    bool Compressed = KlaussCPU::compress(CInst, Inst, getSTI());
    Out.emitInstruction(Compressed ? CInst : Inst, getSTI());
    return false;
  }

  case Match_MissingFeature:
    return Error(IDLoc, "instruction requires an unsupported feature");

  case Match_MnemonicFail:
    return Error(IDLoc, "unrecognised instruction mnemonic");

  default:
    break;
  }

  // An operand did not match: Match_InvalidOperand, or an operand class's
  // DiagnosticString (KlaussCPUInstrInfo.td).  ErrorInfo is the index of the
  // operand; one past the last means an operand is missing, whatever class
  // the best near-miss expected there.
  SMLoc ErrLoc = IDLoc;
  if (ErrorInfo != ~0ULL) {
    if (ErrorInfo >= Operands.size())
      return Error(IDLoc, "too few operands for instruction");
    ErrLoc = Operands[ErrorInfo]->getStartLoc();
  }
  if (Result == Match_InvalidOperand)
    return Error(ErrLoc, "invalid operand for instruction");
  if (const char *Diag =
          getMatchKindDiag(static_cast<KlaussCPUMatchResultTy>(Result)))
    return Error(ErrLoc, Diag);
  llvm_unreachable("unknown match result");
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeKlaussCPUAsmParser() {
  RegisterMCAsmParser<KlaussCPUAsmParser> X(getTheKlaussCPUTarget());
}
