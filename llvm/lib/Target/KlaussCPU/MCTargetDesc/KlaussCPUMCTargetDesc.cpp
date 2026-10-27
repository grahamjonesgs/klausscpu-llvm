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
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
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

std::string KlaussCPU::getEffectiveFeatureString(StringRef FS) {
  std::string Res = FS.str();
  auto Override = [&Res](const cl::opt<bool> &Opt, StringRef Feature) {
    if (!Opt.getNumOccurrences())
      return;
    if (!Res.empty())
      Res += ',';
    Res += (Opt ? "+" : "-") + Feature.str();
  };
  // short-branches implies short-forms, so -short-forms, applied last, turns
  // the branches off too.
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
  MAI->addInitialFrameState(MCCFIInstruction::cfiDefCfa(nullptr, SP, 8));
  MAI->addInitialFrameState(MCCFIInstruction::createOffset(nullptr, RA, -8));
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
}
