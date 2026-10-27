//===-- KlaussCPUMCCompress.cpp - ISA v3 short-form compression -----------===//
//
// ISA v3 short 1-word forms (ISA_V3_PROPOSAL.md §3, A3/A4/A5): a 2-word
// instruction whose immediate fits the 8-bit word-0 field [19:12] is rewritten
// into its 1-word twin (the *_SH defs in KlaussCPUInstrInfo.td). The twins
// have the same operand list, so this is an opcode swap on the MCInst; their
// encodings are the .td Inst fields.
//
// Called from the AsmPrinter (compiled code) and the AsmParser (hand-written
// and inline asm), in the style of RISC-V's RVC compression. Only literal
// immediates compress: a symbolic operand (SETR sym) keeps the long form.
//
// Immediate semantics (must match what the long form computes):
//   SETR / ADDI / CMPRV   sign-extended imm32  -> short SGN=1 simm8
//   ADDV/MINUSV/AND/OR/XORV zero-extended imm32 -> short SGN=0 uimm8
//   LDIDX* / STIDX*       EA = base + imm32    -> simm8 << log2(access size)
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUMCTargetDesc.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Support/CommandLine.h"

#define GET_INSTRINFO_ENUM
#include "KlaussCPUGenInstrInfo.inc"

using namespace llvm;

static cl::opt<bool> EnableShortForms(
    "klausscpu-short-forms", cl::init(true), cl::Hidden,
    cl::desc("Emit ISA v3 short 1-word immediate forms when the value fits"));
static cl::opt<bool> EnableShortBranches(
    "klausscpu-short-branches", cl::init(true), cl::Hidden,
    cl::desc("Emit ISA v3 short 1-word PC-relative branches (relaxed when "
             "out of range)"));

namespace {
enum class ImmKind { SExt, ZExt, Mem };
struct ShortMap {
  unsigned Long, Short;
  ImmKind Kind;
  unsigned ImmOp;  // MCInst operand index of the immediate
  unsigned Shift;  // Mem: log2(access size)
};
} // namespace

static const ShortMap Table[] = {
    {KlaussCPU::SETR,      KlaussCPU::SETR_SH,      ImmKind::SExt, 1, 0},
    {KlaussCPU::ADDI,      KlaussCPU::ADDI_SH,      ImmKind::SExt, 2, 0},
    {KlaussCPU::CMPRV_I,   KlaussCPU::CMPRV_SH,     ImmKind::SExt, 1, 0},
    {KlaussCPU::ADDV,      KlaussCPU::ADDV_SH,      ImmKind::ZExt, 2, 0},
    {KlaussCPU::MINUSV,    KlaussCPU::MINUSV_SH,    ImmKind::ZExt, 2, 0},
    {KlaussCPU::ANDV,      KlaussCPU::ANDV_SH,      ImmKind::ZExt, 2, 0},
    {KlaussCPU::ORV,       KlaussCPU::ORV_SH,       ImmKind::ZExt, 2, 0},
    {KlaussCPU::XORV,      KlaussCPU::XORV_SH,      ImmKind::ZExt, 2, 0},
    {KlaussCPU::LDIDX8,    KlaussCPU::LDIDX8_SH,    ImmKind::Mem,  2, 0},
    {KlaussCPU::LDIDX8_S,  KlaussCPU::LDIDX8_S_SH,  ImmKind::Mem,  2, 0},
    {KlaussCPU::LDIDX16,   KlaussCPU::LDIDX16_SH,   ImmKind::Mem,  2, 1},
    {KlaussCPU::LDIDX16_S, KlaussCPU::LDIDX16_S_SH, ImmKind::Mem,  2, 1},
    {KlaussCPU::LDIDX32,   KlaussCPU::LDIDX32_SH,   ImmKind::Mem,  2, 2},
    {KlaussCPU::LDIDX32_S, KlaussCPU::LDIDX32_S_SH, ImmKind::Mem,  2, 2},
    {KlaussCPU::LDIDX64,   KlaussCPU::LDIDX64_SH,   ImmKind::Mem,  2, 3},
    {KlaussCPU::LDIDX64U,  KlaussCPU::LDIDX64U_SH,  ImmKind::Mem,  2, 3},
    {KlaussCPU::STIDX8,    KlaussCPU::STIDX8_SH,    ImmKind::Mem,  2, 0},
    {KlaussCPU::STIDX16,   KlaussCPU::STIDX16_SH,   ImmKind::Mem,  2, 1},
    {KlaussCPU::STIDX32,   KlaussCPU::STIDX32_SH,   ImmKind::Mem,  2, 2},
    {KlaussCPU::STIDX64,   KlaussCPU::STIDX64_SH,   ImmKind::Mem,  2, 3},
    {KlaussCPU::STIDX64U,  KlaussCPU::STIDX64U_SH,  ImmKind::Mem,  2, 3},
};

static bool fitsS8(int64_t V) { return V >= -128 && V <= 127; }

// A1 short branches: {short, long absolute, long PC-relative (0 = none)}.
namespace {
struct BranchMap {
  unsigned Short, LongAbs, LongRel;
};
} // namespace
static const BranchMap Branches[] = {
    {KlaussCPU::JMP_SH, KlaussCPU::JMP, KlaussCPU::JMPREL},
    {KlaussCPU::JMPZ_SH, KlaussCPU::JMPZ, KlaussCPU::JMPZREL},
    {KlaussCPU::JMPNZ_SH, KlaussCPU::JMPNZ, KlaussCPU::JMPNZREL},
    {KlaussCPU::JMPC_SH, KlaussCPU::JMPC, KlaussCPU::JMPCREL},
    {KlaussCPU::JMPNC_SH, KlaussCPU::JMPNC, KlaussCPU::JMPNCREL},
    {KlaussCPU::JMPO_SH, KlaussCPU::JMPO, 0},
    {KlaussCPU::JMPNO_SH, KlaussCPU::JMPNO, 0},
    {KlaussCPU::JMPS_SH, KlaussCPU::JMPS, KlaussCPU::JMPSREL},
    {KlaussCPU::JMPNS_SH, KlaussCPU::JMPNS, KlaussCPU::JMPNSREL},
    {KlaussCPU::JMPLT_SH, KlaussCPU::JMPLT, KlaussCPU::JMPLTREL},
    {KlaussCPU::JMPGE_SH, KlaussCPU::JMPGE, KlaussCPU::JMPGEREL},
    {KlaussCPU::JMPLE_SH, KlaussCPU::JMPLE, KlaussCPU::JMPLEREL},
    {KlaussCPU::JMPGT_SH, KlaussCPU::JMPGT, KlaussCPU::JMPGTREL},
    {KlaussCPU::JMPULT_SH, KlaussCPU::JMPULT, KlaussCPU::JMPULTREL},
    {KlaussCPU::JMPUGE_SH, KlaussCPU::JMPUGE, KlaussCPU::JMPUGEREL},
    {KlaussCPU::JMPULE_SH, KlaussCPU::JMPULE, KlaussCPU::JMPULEREL},
    {KlaussCPU::JMPUGT_SH, KlaussCPU::JMPUGT, KlaussCPU::JMPUGTREL},
    {KlaussCPU::JMPE_SH, KlaussCPU::JMPE, KlaussCPU::JMPEREL},
    {KlaussCPU::JMPNE_SH, KlaussCPU::JMPNE, KlaussCPU::JMPNEREL},
};

static const BranchMap *findShortBranch(unsigned Opc) {
  for (const BranchMap &B : Branches)
    if (B.Short == Opc)
      return &B;
  return nullptr;
}

int llvm::getKlaussCPUPCRelOperand(unsigned Opcode) {
  switch (Opcode) {
  case KlaussCPU::FBR_RR:
  case KlaussCPU::FBR_RI:
    return 3;
  case KlaussCPU::LEAPC:
    return 1;
  case KlaussCPU::CALLREL:
    return 0;
  default:
    break;
  }
  if (isKlaussCPUShortBranch(Opcode))
    return 0;
  for (const BranchMap &B : Branches)
    if (B.LongRel && B.LongRel == Opcode)
      return 0;
  return -1;
}

bool llvm::isKlaussCPUShortBranch(unsigned Opcode) {
  return findShortBranch(Opcode) != nullptr;
}

unsigned llvm::relaxKlaussCPUShortBranch(unsigned Opcode) {
  const BranchMap *B = findShortBranch(Opcode);
  if (!B)
    return 0;
  return B->LongRel ? B->LongRel : B->LongAbs;
}

bool llvm::compressKlaussCPUInst(MCInst &Inst) {
  if (!EnableShortForms)
    return false;
  // A1: a branch to a symbolic target starts short; the asm backend relaxes
  // it to the 2-word PC-relative form if the target is out of range or not
  // resolvable in-section. Literal (absolute-number) targets stay long.
  if (EnableShortBranches && Inst.getNumOperands() == 1 &&
      Inst.getOperand(0).isExpr()) {
    for (const BranchMap &B : Branches)
      if (Inst.getOpcode() == B.LongAbs || (B.LongRel && Inst.getOpcode() == B.LongRel)) {
        Inst.setOpcode(B.Short);
        return true;
      }
  }
  for (const ShortMap &M : Table) {
    if (Inst.getOpcode() != M.Long)
      continue;
    const MCOperand &MO = Inst.getOperand(M.ImmOp);
    if (!MO.isImm())
      return false; // symbolic: needs the word-1 fixup slot
    // The long forms encode the low 32 bits of the operand (word 1).
    uint32_t Raw = static_cast<uint32_t>(MO.getImm());
    int64_t S = static_cast<int32_t>(Raw);
    bool Fits = false;
    switch (M.Kind) {
    case ImmKind::SExt:
      Fits = fitsS8(S);
      break;
    case ImmKind::ZExt:
      Fits = Raw <= 0xFF;
      break;
    case ImmKind::Mem: {
      int64_t Scale = int64_t(1) << M.Shift;
      Fits = (S % Scale) == 0 && fitsS8(S / Scale);
      break;
    }
    }
    if (!Fits)
      return false;
    Inst.setOpcode(M.Short);
    return true;
  }
  return false;
}
