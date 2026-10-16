//===-- KlaussCPUDisassembler.cpp - Disassembler for KlaussCPU -----------===//
//
// Decodes ISA encoding v2/v3 instruction words (see KlaussCPUMCCodeEmitter.cpp
// for the layout).  The encoder is hand-written, so the decode table
// (KlaussCPUGenDecoderTable.inc) is generated from the encoder's `case` lines
// by gen-decoder-table.py rather than by -gen-disassembler.
//
// Instructions are 1–3 little-endian 32-bit words; LEN = word0[31:30]
// (01 = 1 word, 10 = 2, 11 = 3).  LEN = 00 (retired v1 opcodes, data) does
// not decode.
//
// PC-relative branch operands (short A1 branches, fused compare-and-branch,
// the 2-word *REL forms, LEAPC) decode to the byte displacement from the
// instruction's address, which is what the encoder takes for an immediate
// operand; KlaussCPUInstPrinter / KlaussCPUMCInstrAnalysis add the address
// back when printing or following a branch.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/KlaussCPUMCTargetDesc.h"
#include "TargetInfo/KlaussCPUTargetInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDecoderOps.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

#define GET_INSTRINFO_ENUM
#include "KlaussCPUGenInstrInfo.inc"

#define GET_REGINFO_ENUM
#include "KlaussCPUGenRegisterInfo.inc"

using namespace llvm;

#define DEBUG_TYPE "klausscpu-disassembler"

typedef MCDisassembler::DecodeStatus DecodeStatus;

namespace {

// Operand field kinds (see gen-decoder-table.py).
enum FieldKind : uint8_t {
  F_RD,       // word0[11:8]  register
  F_RS1,      // word0[7:4]   register
  F_RS2,      // word0[3:0]   register
  F_N6,       // word0[20:15] unsigned shift / bit index
  F_ENTER22,  // word0[21:0]  ENTER frame size in 8-byte units
  F_IMM4,     // word0[3:0]   fused-branch signed immediate
  F_FBRCC,    // word0[25:22] fused-branch condition: PRED[25:23] | INV[22]<<3
  F_PC13,     // word0[20:8]  fused-branch signed word displacement
  F_PC18,     // word0[17:0]  short-branch signed word displacement
  F_SH_S,     // word0[19:12] short-form signed imm8
  F_SH_Z,     // word0[19:12] short-form unsigned imm8
  F_SH_M0,    // word0[19:12] short-form signed imm8, memory offset << 0
  F_SH_M1,    //                                          ... << 1
  F_SH_M2,    //                                          ... << 2
  F_SH_M3,    //                                          ... << 3
  F_W1S,      // word1 signed imm32
  F_W1U,      // word1 unsigned imm32 (absolute address)
  F_W1PC,     // word1 signed PC-relative byte displacement
  F_W2S,      // word2 signed imm32 (high half of a 64-bit immediate)
};

struct Field {
  FieldKind Kind;
  uint8_t OpIdx;
};

struct DecodeEntry {
  unsigned Opcode;
  uint8_t Words;
  uint32_t Template;
  uint32_t Mask;
  uint8_t NumFields;
  Field Fields[4];
};

const DecodeEntry DecodeTable[] = {
#include "KlaussCPUGenDecoderTable.inc"
};

const MCPhysReg GPRs[16] = {
    KlaussCPU::R0,  KlaussCPU::R1,  KlaussCPU::R2,  KlaussCPU::R3,
    KlaussCPU::R4,  KlaussCPU::R5,  KlaussCPU::R6,  KlaussCPU::R7,
    KlaussCPU::R8,  KlaussCPU::R9,  KlaussCPU::R10, KlaussCPU::R11,
    KlaussCPU::R12, KlaussCPU::R13, KlaussCPU::R14, KlaussCPU::R15};

class KlaussCPUDisassembler : public MCDisassembler {
  std::unique_ptr<const MCInstrInfo> MCII;

public:
  KlaussCPUDisassembler(const MCSubtargetInfo &STI, MCContext &Ctx,
                        const MCInstrInfo *MCII)
      : MCDisassembler(STI, Ctx), MCII(MCII) {}

  DecodeStatus getInstruction(MCInst &MI, uint64_t &Size,
                              ArrayRef<uint8_t> Bytes, uint64_t Address,
                              raw_ostream &CStream) const override;
};

} // end anonymous namespace

static MCDisassembler *createKlaussCPUDisassembler(const Target &T,
                                                   const MCSubtargetInfo &STI,
                                                   MCContext &Ctx) {
  return new KlaussCPUDisassembler(STI, Ctx, T.createMCInstrInfo());
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeKlaussCPUDisassembler() {
  TargetRegistry::RegisterMCDisassembler(getTheKlaussCPUTarget(),
                                         createKlaussCPUDisassembler);
}

static MCOperand decodeField(FieldKind Kind, uint32_t W0, uint32_t W1,
                             uint32_t W2) {
  switch (Kind) {
  case F_RD:
    return MCOperand::createReg(GPRs[(W0 >> 8) & 0xF]);
  case F_RS1:
    return MCOperand::createReg(GPRs[(W0 >> 4) & 0xF]);
  case F_RS2:
    return MCOperand::createReg(GPRs[W0 & 0xF]);
  case F_N6:
    return MCOperand::createImm((W0 >> 15) & 0x3F);
  case F_ENTER22:
    return MCOperand::createImm(W0 & 0x3FFFFF);
  case F_IMM4:
    return MCOperand::createImm(SignExtend64<4>(W0 & 0xF));
  case F_FBRCC:
    return MCOperand::createImm(((W0 >> 23) & 7) | (((W0 >> 22) & 1) << 3));
  case F_PC13:
    return MCOperand::createImm(SignExtend64<13>((W0 >> 8) & 0x1FFF) * 4);
  case F_PC18:
    return MCOperand::createImm(SignExtend64<18>(W0 & 0x3FFFF) * 4);
  case F_SH_S:
    return MCOperand::createImm(SignExtend64<8>((W0 >> 12) & 0xFF));
  case F_SH_Z:
    return MCOperand::createImm((W0 >> 12) & 0xFF);
  case F_SH_M0:
  case F_SH_M1:
  case F_SH_M2:
  case F_SH_M3:
    return MCOperand::createImm(SignExtend64<8>((W0 >> 12) & 0xFF)
                                << (Kind - F_SH_M0));
  case F_W1S:
  case F_W1PC:
    return MCOperand::createImm(SignExtend64<32>(W1));
  case F_W1U:
    return MCOperand::createImm(W1);
  case F_W2S:
    return MCOperand::createImm(SignExtend64<32>(W2));
  }
  llvm_unreachable("unknown field kind");
}

DecodeStatus KlaussCPUDisassembler::getInstruction(MCInst &MI, uint64_t &Size,
                                                   ArrayRef<uint8_t> Bytes,
                                                   uint64_t Address,
                                                   raw_ostream &CS) const {
  Size = 0;
  if (Bytes.size() < 4)
    return Fail;
  uint32_t W0 = support::endian::read32le(Bytes.data());
  unsigned Words = W0 >> 30;
  if (Words == 0) {
    // Retired v1 opcode or data: consume one word so disassembly continues.
    Size = 4;
    return Fail;
  }
  if (Bytes.size() < Words * 4) {
    Size = Bytes.size() & ~uint64_t(3);
    return Fail;
  }
  // Every result below, including failures, consumes the whole instruction
  // so disassembly stays word-aligned.
  Size = Words * 4;
  uint32_t W1 = Words > 1 ? support::endian::read32le(Bytes.data() + 4) : 0;
  uint32_t W2 = Words > 2 ? support::endian::read32le(Bytes.data() + 8) : 0;

  for (const DecodeEntry &E : DecodeTable) {
    if (E.Words != Words || (W0 & E.Mask) != E.Template)
      continue;

    // Place each field at its .td operand index, then fill tied operands
    // (e.g. SETFR's $rs1 tied to $rd) from the operand they are tied to.
    const MCInstrDesc &Desc = MCII->get(E.Opcode);
    unsigned NumOps = Desc.getNumOperands();
    SmallVector<MCOperand, 6> Ops(NumOps);
    SmallVector<bool, 6> Set(NumOps, false);
    for (unsigned I = 0; I < E.NumFields; ++I) {
      const Field &F = E.Fields[I];
      if (F.OpIdx >= NumOps)
        return Fail;
      Ops[F.OpIdx] = decodeField(F.Kind, W0, W1, W2);
      Set[F.OpIdx] = true;
    }
    // Tied operands decoded from two fields must agree: the assembly syntax
    // has one slot for them, so a word with rd != rs1 is not representable.
    for (unsigned I = 0; I < NumOps; ++I) {
      int Tied = Desc.getOperandConstraint(I, MCOI::TIED_TO);
      if (Set[I]) {
        if (Tied >= 0 && Set[Tied] && Ops[I].isReg() &&
            Ops[I].getReg() != Ops[Tied].getReg())
          return Fail;
        continue;
      }
      if (Tied < 0 || !Set[Tied])
        return Fail;
      Ops[I] = Ops[Tied];
    }

    MI.clear();
    MI.setOpcode(E.Opcode);
    for (const MCOperand &Op : Ops)
      MI.addOperand(Op);
    return Success;
  }

  return Fail;
}
