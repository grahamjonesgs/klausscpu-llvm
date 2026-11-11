//===-- KlaussCPUMCTargetDesc.cpp - KlaussCPU Target Descriptions ----------===//
//
// KlaussCPU LLVM backend — MC component registration.
//
// Registers:
//   MCAsmInfo        — assembler syntax and directives
//   MCInstrInfo      — instruction descriptions (from tablegen)
//   MCRegisterInfo   — register descriptions (from tablegen)
//   MCSubtargetInfo  — subtarget feature descriptions (from tablegen)
//   MCInstPrinter    — text assembly printer
//
// and holds the generated CompressPat functions (KlaussCPU::compress).
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUInstPrinter.h"
#include "KlaussCPUMCAsmInfo.h"
#include "KlaussCPUMCTargetDesc.h"
#include "TargetInfo/KlaussCPUTargetInfo.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCELFStreamer.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/TargetParser/KlaussCPUABI.h"
#include "llvm/TargetParser/Triple.h"

// Register enum must come first — instruction descriptions reference register
// identifiers (e.g. KlaussCPU::SP) in their implicit Uses/Defs tables.
#define GET_REGINFO_ENUM
#include "KlaussCPUGenRegisterInfo.inc"

#define GET_INSTRINFO_ENUM
#include "KlaussCPUGenInstrInfo.inc"

#define GET_INSTRINFO_MC_DESC
#define ENABLE_INSTR_PREDICATE_VERIFIER
#include "KlaussCPUGenInstrInfo.inc"

#define GET_SUBTARGETINFO_MC_DESC
#include "KlaussCPUGenSubtargetInfo.inc"

#define GET_REGINFO_MC_DESC
#include "KlaussCPUGenRegisterInfo.inc"

using namespace llvm;

#define GEN_COMPRESS_INSTR
#define GEN_UNCOMPRESS_INSTR
#include "KlaussCPUGenCompressInstEmitter.inc"

bool KlaussCPU::compress(MCInst &OutInst, const MCInst &MI,
                         const MCSubtargetInfo &STI) {
  return compressInst(OutInst, MI, STI);
}

bool KlaussCPU::uncompress(MCInst &OutInst, const MCInst &MI,
                           const MCSubtargetInfo &STI) {
  return uncompressInst(OutInst, MI, STI);
}

// System registers of MFSR / MTSR and their assembler names (klaussCPU
// ISA_ENCODING_V2_MAP §9.1, §10.1; MMU_PLAN.md §6.1, §10.1, §11.3).  A
// number missing here has no name: the assembler takes it as a number and
// the printer prints it as one.
namespace {
struct SysRegEntry {
  unsigned Num;
  StringLiteral Name; // lower case
};
} // namespace

// clang-format off: one MPU region per line.
static constexpr SysRegEntry SysRegs[] = {
    // Stage 1 (MMU_PLAN §6.1).
    {0x00, "status"},   {0x01, "evec"},     {0x02, "ecause"},
    {0x03, "eaddr"},    {0x04, "usp"},      {0x05, "scratch0"},
    {0x06, "scratch1"}, {0x07, "tp"},       {0x08, "cycle"},
    // Stage 2, MPU (MMU_PLAN §10.1): region n at 0x10 + 2n / 0x11 + 2n.
    {0x10, "mpu_base0"},   {0x11, "mpu_limit0"},
    {0x12, "mpu_base1"},   {0x13, "mpu_limit1"},
    {0x14, "mpu_base2"},   {0x15, "mpu_limit2"},
    {0x16, "mpu_base3"},   {0x17, "mpu_limit3"},
    {0x18, "mpu_base4"},   {0x19, "mpu_limit4"},
    {0x1A, "mpu_base5"},   {0x1B, "mpu_limit5"},
    {0x1C, "mpu_base6"},   {0x1D, "mpu_limit6"},
    {0x1E, "mpu_base7"},   {0x1F, "mpu_limit7"},
    {0x20, "mpu_base8"},   {0x21, "mpu_limit8"},
    {0x22, "mpu_base9"},   {0x23, "mpu_limit9"},
    {0x24, "mpu_base10"},  {0x25, "mpu_limit10"},
    {0x26, "mpu_base11"},  {0x27, "mpu_limit11"},
    {0x28, "mpu_base12"},  {0x29, "mpu_limit12"},
    {0x2A, "mpu_base13"},  {0x2B, "mpu_limit13"},
    {0x2C, "mpu_base14"},  {0x2D, "mpu_limit14"},
    {0x2E, "mpu_base15"},  {0x2F, "mpu_limit15"},
    // Stage 3, TLB (MMU_PLAN §11.3).
    {0x30, "tlb_hi"},   {0x31, "tlb_lo"},   {0x32, "tlb_index"},
    {0x33, "asid"},     {0x34, "tlbvec"},
};
// clang-format on

StringRef KlaussCPU::getSysRegName(unsigned Num) {
  for (const SysRegEntry &E : SysRegs)
    if (E.Num == Num)
      return E.Name;
  return StringRef();
}

std::optional<unsigned> KlaussCPU::lookupSysReg(StringRef Name) {
  for (const SysRegEntry &E : SysRegs)
    if (Name.equals_insensitive(E.Name))
      return E.Num;
  return std::nullopt;
}

StringRef KlaussCPU::getSpecifierName(uint16_t S) {
  switch (S) {
  case S_TPREL:
    return "tprel";
  case S_GOTPCREL:
    return "gotpcrel";
  case S_PLT:
    return "plt";
  case S_GOTTPREL:
    return "gottprel";
  case S_TLSGD:
    return "tlsgd";
  default:
    return "";
  }
}

uint16_t KlaussCPU::parseSpecifierName(StringRef Name) {
  for (uint16_t S : {S_TPREL, S_GOTPCREL, S_PLT, S_GOTTPREL, S_TLSGD})
    if (Name.equals_insensitive(getSpecifierName(S)))
      return S;
  return S_None;
}

bool KlaussCPU::isSpecifierAllowed(unsigned Opcode, uint16_t S) {
  switch (S) {
  case S_GOTPCREL:
  case S_GOTTPREL:
  case S_TLSGD:
    return Opcode == KlaussCPU::LEAPC;
  case S_PLT:
    return Opcode == KlaussCPU::CALLREL || Opcode == KlaussCPU::JMPREL ||
           Opcode == KlaussCPU::TAILREL;
  default:
    return false;
  }
}

uint16_t KlaussCPU::getExprSpecifier(const MCExpr *Expr) {
  switch (Expr->getKind()) {
  case MCExpr::Specifier:
    return cast<MCSpecifierExpr>(Expr)->getSpecifier();
  case MCExpr::Unary:
    return getExprSpecifier(cast<MCUnaryExpr>(Expr)->getSubExpr());
  case MCExpr::Binary: {
    const auto *BE = cast<MCBinaryExpr>(Expr);
    if (uint16_t S = getExprSpecifier(BE->getLHS()))
      return S;
    return getExprSpecifier(BE->getRHS());
  }
  default:
    return S_None;
  }
}

// A/B switches for the ISA v3 short forms, on by default in every processor.
// Given explicitly they override -mattr.
static cl::opt<bool> EnableShortForms(
    "klausscpu-short-forms", cl::init(true), cl::Hidden,
    cl::desc("Emit ISA v3 short 1-word immediate forms when the value fits "
             "(feature short-forms)"));
static cl::opt<bool> EnableShortBranches(
    "klausscpu-short-branches", cl::init(true), cl::Hidden,
    cl::desc("Emit ISA v3 short 1-word PC-relative branches, relaxed when "
             "out of range (feature short-branches)"));

static cl::opt<bool> EnableShortCalls(
    "klausscpu-short-calls", cl::init(true), cl::Hidden,
    cl::desc("Emit ISA v3 short 1-word PC-relative calls, resolved by the "
             "linker through a thunk when out of range (feature short-calls)"));

std::string KlaussCPU::getEffectiveFeatureString(StringRef FS) {
  std::string Res = FS.str();
  auto Override = [&Res](const cl::opt<bool> &Opt, StringRef Feature) {
    if (!Opt.getNumOccurrences())
      return;
    if (!Res.empty())
      Res += ',';
    Res += (Opt ? "+" : "-") + Feature.str();
  };
  // short-calls implies short-branches implies short-forms, so each one
  // applied later turns the ones before it off too.
  Override(EnableShortCalls, "short-calls");
  Override(EnableShortBranches, "short-branches");
  Override(EnableShortForms, "short-forms");
  return Res;
}

static MCInstrInfo *createKlaussCPUMCInstrInfo() {
  MCInstrInfo *X = new MCInstrInfo();
  InitKlaussCPUMCInstrInfo(X);
  return X;
}

static MCRegisterInfo *createKlaussCPUMCRegisterInfo(const Triple & /*TT*/) {
  MCRegisterInfo *X = new MCRegisterInfo();
  // RA = virtual DWARF-only column 17; KlaussCPU has no hardware link register.
  InitKlaussCPUMCRegisterInfo(X, KlaussCPU::RA);
  return X;
}

// Initial CFI state at a function's first instruction: CALL has just pushed
// the 64-bit return address, so the caller's SP (the CFA) is SP+8 and the
// return-address column (RA, DWARF 17) is saved at CFA-8.
static MCAsmInfo *createKlaussCPUMCAsmInfo(const MCRegisterInfo &MRI,
                                           const Triple &TT,
                                           const MCTargetOptions &Options) {
  MCAsmInfo *MAI = new KlaussCPUMCAsmInfo(TT, Options);
  unsigned SP = MRI.getDwarfRegNum(KlaussCPU::SP, true);
  unsigned RA = MRI.getDwarfRegNum(KlaussCPU::RA, true);
  constexpr int RetAddrSize = KlaussCPUABI::RetAddrSize;
  MAI->addInitialFrameState(
      MCCFIInstruction::cfiDefCfa(nullptr, SP, RetAddrSize));
  MAI->addInitialFrameState(
      MCCFIInstruction::createOffset(nullptr, RA, -RetAddrSize));
  return MAI;
}

static MCSubtargetInfo *createKlaussCPUMCSubtargetInfo(const Triple &TT,
                                                        StringRef CPU,
                                                        StringRef FS) {
  std::string CPUName = std::string(CPU);
  if (CPUName.empty())
    CPUName = "generic";
  return createKlaussCPUMCSubtargetInfoImpl(
      TT, CPUName, /*TuneCPU=*/CPUName,
      KlaussCPU::getEffectiveFeatureString(FS));
}

static MCInstPrinter *
createKlaussCPUMCInstPrinter(const Triple & /*T*/, unsigned SyntaxVariant,
                               const MCAsmInfo &MAI, const MCInstrInfo &MII,
                               const MCRegisterInfo &MRI) {
  if (SyntaxVariant == 0)
    return new KlaussCPUInstPrinter(MAI, MII, MRI);
  return nullptr;
}

// Index of the PC-relative operand of Desc (a byte displacement from the
// instruction's address when it is an immediate), or -1 if it has none.
static int getPCRelOperand(const MCInstrDesc &Desc) {
  for (auto [I, Op] : enumerate(Desc.operands()))
    if (Op.OperandType == MCOI::OPERAND_PCREL)
      return I;
  return -1;
}

namespace {
// Branch-target evaluation for llvm-objdump symbolization and debuggers.
class KlaussCPUMCInstrAnalysis : public MCInstrAnalysis {
public:
  explicit KlaussCPUMCInstrAnalysis(const MCInstrInfo *Info)
      : MCInstrAnalysis(Info) {}

  bool evaluateBranch(const MCInst &Inst, uint64_t Addr, uint64_t Size,
                      uint64_t &Target) const override {
    const MCInstrDesc &Desc = Info->get(Inst.getOpcode());
    if (!(Desc.isBranch() || Desc.isCall()))
      return false; // includes LEAPC, whose PC-relative operand is data
    int PCRelOp = getPCRelOperand(Desc);
    unsigned TargetOp = PCRelOp >= 0 ? PCRelOp : 0;
    if (Inst.getNumOperands() <= TargetOp || !Inst.getOperand(TargetOp).isImm())
      return false; // register-indirect (JMPR/CALLR) or symbolic
    int64_t Imm = Inst.getOperand(TargetOp).getImm();
    Target = uint32_t(PCRelOp >= 0 ? Addr + Imm : Imm);
    return true;
  }
};
} // namespace

static MCInstrAnalysis *createKlaussCPUInstrAnalysis(const MCInstrInfo *Info) {
  return new KlaussCPUMCInstrAnalysis(Info);
}

namespace {
// Marks every ELF object with the calling convention its code follows
// (KlaussCPUABI::ABIVersion in e_flags), so lld can refuse to link code built
// for an older one: an object compiled before the ABI change and left behind
// by a build system would otherwise fail at run time.
class KlaussCPUTargetELFStreamer : public MCTargetStreamer {
public:
  explicit KlaussCPUTargetELFStreamer(MCStreamer &S) : MCTargetStreamer(S) {}
  void finish() override {
    ELFObjectWriter &W = static_cast<MCELFStreamer &>(Streamer).getWriter();
    W.setELFHeaderEFlags((W.getELFHeaderEFlags() &
                          ~KlaussCPUABI::EFlagsABIMask) |
                         KlaussCPUABI::ABIVersion);
  }
};
} // namespace

static MCTargetStreamer *
createKlaussCPUObjectTargetStreamer(MCStreamer &S, const MCSubtargetInfo &STI) {
  if (STI.getTargetTriple().isOSBinFormatELF())
    return new KlaussCPUTargetELFStreamer(S);
  return nullptr;
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeKlaussCPUTargetMC() {
  Target &T = getTheKlaussCPUTarget();

  // Assembler info (comment string, directive syntax, etc.)
  TargetRegistry::RegisterMCAsmInfo(T, createKlaussCPUMCAsmInfo);

  // Instruction and register descriptions generated by tablegen.
  TargetRegistry::RegisterMCInstrInfo(T, createKlaussCPUMCInstrInfo);
  TargetRegistry::RegisterMCRegInfo(T, createKlaussCPUMCRegisterInfo);
  TargetRegistry::RegisterMCSubtargetInfo(T, createKlaussCPUMCSubtargetInfo);

  // Text assembly printer.
  TargetRegistry::RegisterMCInstPrinter(T, createKlaussCPUMCInstPrinter);
  TargetRegistry::RegisterMCInstrAnalysis(T, createKlaussCPUInstrAnalysis);

  // Code emitter + assembler backend: enables -filetype=obj ELF output.
  TargetRegistry::RegisterMCCodeEmitter(T, createKlaussCPUMCCodeEmitter);
  TargetRegistry::RegisterMCAsmBackend(T, createKlaussCPUAsmBackend);
  TargetRegistry::RegisterObjectTargetStreamer(
      T, createKlaussCPUObjectTargetStreamer);
}
