//===-- KlaussCPUDisassembler.cpp - Disassembler for KlaussCPU -----------===//
//
// Decodes ISA encoding v2/v3 instructions with the tables TableGen generates
// from the .td `Inst` fields (KlaussCPUGenDisassemblerTables.inc).
//
// Instructions are 1–3 little-endian 32-bit words; LEN = word0[31:30]
// (01 = 1 word, 10 = 2, 11 = 3) selects the table.  LEN = 00 (retired v1
// opcodes, data) does not decode.
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
#include "llvm/MC/MCDecoder.h"
#include "llvm/MC/MCDecoderOps.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"
#include <bitset>

#define GET_INSTRINFO_ENUM
#include "KlaussCPUGenInstrInfo.inc"

#define GET_REGINFO_ENUM
#include "KlaussCPUGenRegisterInfo.inc"

using namespace llvm;
using namespace llvm::MCD;

#define DEBUG_TYPE "klausscpu-disassembler"

typedef MCDisassembler::DecodeStatus DecodeStatus;

namespace {

class KlaussCPUDisassembler : public MCDisassembler {
public:
  KlaussCPUDisassembler(const MCSubtargetInfo &STI, MCContext &Ctx)
      : MCDisassembler(STI, Ctx) {}

  DecodeStatus getInstruction(MCInst &MI, uint64_t &Size,
                              ArrayRef<uint8_t> Bytes, uint64_t Address,
                              raw_ostream &CStream) const override;
};

} // end anonymous namespace

static MCDisassembler *createKlaussCPUDisassembler(const Target &T,
                                                   const MCSubtargetInfo &STI,
                                                   MCContext &Ctx) {
  return new KlaussCPUDisassembler(STI, Ctx);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeKlaussCPUDisassembler() {
  TargetRegistry::RegisterMCDisassembler(getTheKlaussCPUTarget(),
                                         createKlaussCPUDisassembler);
}

static DecodeStatus DecodeGPRRegisterClass(MCInst &Inst, uint32_t RegNo,
                                           uint64_t Address,
                                           const MCDisassembler *Decoder) {
  assert(RegNo < 16 && "register field is 4 bits");
  Inst.addOperand(MCOperand::createReg(KlaussCPU::R0 + RegNo));
  return MCDisassembler::Success;
}

template <unsigned N>
static DecodeStatus decodeUImmOperand(MCInst &Inst, uint64_t Imm,
                                      int64_t Address,
                                      const MCDisassembler *Decoder) {
  assert(isUInt<N>(Imm) && "Invalid immediate");
  Inst.addOperand(MCOperand::createImm(Imm));
  return MCDisassembler::Success;
}

template <unsigned N>
static DecodeStatus decodeSImmOperand(MCInst &Inst, uint64_t Imm,
                                      int64_t Address,
                                      const MCDisassembler *Decoder) {
  assert(isUInt<N>(Imm) && "Invalid immediate");
  Inst.addOperand(MCOperand::createImm(SignExtend64<N>(Imm)));
  return MCDisassembler::Success;
}

// Fused-branch condition PRED | INV << 3: the CPU implements PRED 0..4
// (EQ, LT, LE, ULT, ULE) and traps on 5..7.
static DecodeStatus decodeFBRCondOperand(MCInst &Inst, uint64_t Imm,
                                         int64_t Address,
                                         const MCDisassembler *Decoder) {
  assert(isUInt<4>(Imm) && "Invalid immediate");
  if ((Imm & 7) > 4)
    return MCDisassembler::Fail;
  Inst.addOperand(MCOperand::createImm(Imm));
  return MCDisassembler::Success;
}

// BEXTR / BDEP: $rd is tied to $rs, but both rd [11:8] and rs1 [7:4] are
// encoded, so only words with rd == rs1 have an assembly spelling.
static DecodeStatus decodeTiedRdRsImm32(MCInst &Inst, uint64_t Insn,
                                        uint64_t Address,
                                        const MCDisassembler *Decoder) {
  uint32_t Rd = fieldFromInstruction(Insn, 8, 4);
  if (fieldFromInstruction(Insn, 4, 4) != Rd)
    return MCDisassembler::Fail;
  DecodeGPRRegisterClass(Inst, Rd, Address, Decoder);
  DecodeGPRRegisterClass(Inst, Rd, Address, Decoder);
  return decodeSImmOperand<32>(Inst, fieldFromInstruction(Insn, 32, 32),
                               Address, Decoder);
}

#include "KlaussCPUGenDisassemblerTables.inc"

namespace {
// Define bitwidths for the types used to instantiate the decoder.
template <> constexpr uint32_t InsnBitWidth<uint32_t> = 32;
template <> constexpr uint32_t InsnBitWidth<uint64_t> = 64;
template <> constexpr uint32_t InsnBitWidth<std::bitset<96>> = 96;
} // namespace

DecodeStatus KlaussCPUDisassembler::getInstruction(MCInst &MI, uint64_t &Size,
                                                   ArrayRef<uint8_t> Bytes,
                                                   uint64_t Address,
                                                   raw_ostream &CS) const {
  using namespace support;
  Size = 0;
  if (Bytes.size() < 4)
    return MCDisassembler::Fail;
  uint32_t W0 = endian::read32le(Bytes.data());
  unsigned Words = W0 >> 30;
  if (Words == 0) {
    // Retired v1 opcode or data: consume one word so disassembly continues.
    Size = 4;
    return MCDisassembler::Fail;
  }
  if (Bytes.size() < Words * 4) {
    Size = Bytes.size() & ~uint64_t(3);
    return MCDisassembler::Fail;
  }
  // Every result below, including failures, consumes the whole instruction
  // so disassembly stays word-aligned.
  Size = Words * 4;
  switch (Words) {
  case 1:
    return decodeInstruction(DecoderTableKlaussCPU32, MI, W0, Address, this,
                             STI);
  case 2:
    return decodeInstruction(DecoderTableKlaussCPU64, MI,
                             endian::read64le(Bytes.data()), Address, this,
                             STI);
  default: {
    std::bitset<96> Insn(endian::read64le(Bytes.data()));
    Insn |= std::bitset<96>(endian::read32le(Bytes.data() + 8)) << 64;
    return decodeInstruction(DecoderTableKlaussCPU96, MI, Insn, Address, this,
                             STI);
  }
  }
}
