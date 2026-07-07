//===-- KlaussCPUMCCodeEmitter.cpp - KlaussCPU instruction encoder ---------===//
//
// Converts MCInst to raw bytes for ELF object output — ISA encoding **v2**
// (the flag-day renumbering, ISA_ENCODING_V2.md).  There is no
// -gen-emitter, so this file is the sole source of truth for instruction
// bytes; the `let Inst{}` fields in KlaussCPUInstrInfo.td are documentary.
//
// All instruction words are written little-endian (least-significant byte
// first), matching the KlaussCPU LE physical memory / instruction-fetch unit.
//
// v2 word-0 layout (every instruction):
//
//   31 30 29    26 25              16 15  12 11  8 7   4 3   0
//   ┌─────┬───────┬──────────────────┬──────┬─────┬─────┬─────┐
//   │ LEN │ CLASS │ attributes + OP  │  x   │ rd  │ rs1 │ rs2 │
//   └─────┴───────┴──────────────────┴──────┴─────┴─────┴─────┘
//
//   LEN[31:30]  01=1 word, 10=2 words, 11=3 words.
//   rd [11:8]   destination (loads: dest; STORES: data source).
//   rs1[7:4]    first source / base address.
//   rs2[3:0]    second source / shift count reg / indirect branch target.
//
// The "template" for each instruction is the full 32-bit word 0 with all
// register fields = 0 (LEN + CLASS + attribute/OP bits already baked in).
// To assemble:  word0 = template | rd<<8 | rs1<<4 | rs2  (+ N<<15 for the
// class-4 embedded-count forms).  Unused register/x fields MUST stay 0 — the
// v2 CPU matches them strictly and traps otherwise.
//
// imm32 lives at PC+4 (word 1) of a 2-word instruction; imm64 is lo32 at PC+4,
// hi32 at PC+8 of a 3-word instruction.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUFixupKinds.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#define GET_INSTRINFO_ENUM
#include "KlaussCPUGenInstrInfo.inc"

#define GET_REGINFO_ENUM
#include "KlaussCPUGenRegisterInfo.inc"

using namespace llvm;

namespace {

class KlaussCPUMCCodeEmitter : public MCCodeEmitter {
  const MCInstrInfo &MCII;
  MCContext &Ctx;

public:
  KlaussCPUMCCodeEmitter(const MCInstrInfo &MCII, MCContext &Ctx)
      : MCII(MCII), Ctx(Ctx) {}

  void encodeInstruction(const MCInst &MI, SmallVectorImpl<char> &CB,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const override;

private:
  // Hardware 4-bit register encoding (0–15) for MI.getOperand(OpNo).
  unsigned getReg(const MCInst &MI, unsigned OpNo) const {
    return Ctx.getRegisterInfo()->getEncodingValue(
        MI.getOperand(OpNo).getReg());
  }

  // Lower 32 bits of an immediate operand (handles signed sign-extension).
  static uint32_t getImm32(const MCInst &MI, unsigned OpNo) {
    return static_cast<uint32_t>(MI.getOperand(OpNo).getImm());
  }

  // Emit a 32-bit value little-endian (LSB at lowest address).
  static void emitLE32(uint32_t V, SmallVectorImpl<char> &CB) {
    CB.push_back(static_cast<char>((V >>  0) & 0xFF));
    CB.push_back(static_cast<char>((V >>  8) & 0xFF));
    CB.push_back(static_cast<char>((V >> 16) & 0xFF));
    CB.push_back(static_cast<char>((V >> 24) & 0xFF));
  }

  // ── v2 register-field placement (uniform across all instructions) ─────────
  //   rd  -> [11:8]   rs1 -> [7:4]   rs2 -> [3:0]
  // Helpers name the *v2 fields* they fill, and take the MCInst operand index
  // whose register goes there.  Everything not named is left 0.

  uint32_t fRd (const MCInst &MI, unsigned Op) const { return getReg(MI, Op) << 8; }
  uint32_t fRs1(const MCInst &MI, unsigned Op) const { return getReg(MI, Op) << 4; }
  uint32_t fRs2(const MCInst &MI, unsigned Op) const { return getReg(MI, Op) << 0; }

  // Combine a template with a lo-32 immediate into the 64-bit value the
  // top-level packer splits into word0 (bytes 0-3) then word1 (bytes 4-7).
  static uint64_t pack2(uint32_t W0, uint32_t W1) {
    return (static_cast<uint64_t>(W0) << 32) | W1;
  }

  // 2-word instruction whose word-1 immediate may be a symbol.  Emits the
  // given fixup at byte offset 4 (word 1) when the operand is an MCExpr.
  uint64_t pack2Sym(uint32_t W0, const MCInst &MI, unsigned ImmOp,
                    KlaussCPU::Fixups FK,
                    SmallVectorImpl<MCFixup> &Fixups) const {
    const MCOperand &MO = MI.getOperand(ImmOp);
    if (MO.isImm())
      return pack2(W0, static_cast<uint32_t>(MO.getImm()));
    assert(MO.isExpr() && "operand must be imm or expr");
    Fixups.push_back(MCFixup::create(4, MO.getExpr(), MCFixupKind(FK)));
    return pack2(W0, 0);
  }

  uint32_t encode32(const MCInst &MI) const;
  uint64_t encode64(const MCInst &MI, SmallVectorImpl<MCFixup> &Fixups) const;
};

} // anonymous namespace

//===----------------------------------------------------------------------===//
// 4-byte (1-word) instruction dispatch
//===----------------------------------------------------------------------===//

uint32_t KlaussCPUMCCodeEmitter::encode32(const MCInst &MI) const {
  switch (MI.getOpcode()) {

  // ── Class 1/A/4/3 RRR: rd = rs1 OP rs2  (rd,rs1,rs2 at ops 0,1,2) ─────────
  case KlaussCPU::ADDR:  return 0x44200000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::SUBR:  return 0x44600000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::ADDC:  return 0x44A00000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::SUBC:  return 0x44E00000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::ANDR:  return 0x45000000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::ORR:   return 0x45400000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::XORR:  return 0x45800000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::MINR:  return 0x45C00000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::MAXR:  return 0x46000000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::MINUR: return 0x46400000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::MAXUR: return 0x46800000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);

  case KlaussCPU::MULUR:  return 0x68000000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::MULHUR: return 0x68400000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::MULR:   return 0x68800000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::MULHR:  return 0x68C00000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::DIVUR:  return 0x69000000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::DIVR:   return 0x69800000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::MODUR:  return 0x6A000000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::MODR:   return 0x6A800000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);

  // Class 4 shifts/rotates, register count = rs2[5:0].
  case KlaussCPU::SHLR:   return 0x50004000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::SHRR:   return 0x50404000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::SARR:   return 0x50804000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::ROLR:   return 0x50C04000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::RORR_R: return 0x51004000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);

  // Class 4 bit ops, register position = rs2[5:0].
  case KlaussCPU::BSETRR: return 0x52000000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::BCLRRR: return 0x52400000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::BTGLRR: return 0x52800000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::BTSTRR: return 0x52C00000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);

  // Class 3 boolean compare: rd = (rs1 cond rs2).
  case KlaussCPU::CMPEQR:  return 0x4C200000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::CMPNER:  return 0x4C600000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::CMPLTR:  return 0x4CA00000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::CMPGER:  return 0x4CE00000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::CMPLER:  return 0x4D200000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::CMPGTR:  return 0x4D600000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::CMPULTR: return 0x4DA00000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::CMPUGER: return 0x4DE00000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::CMPULER: return 0x4E200000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);
  case KlaussCPU::CMPUGTR: return 0x4E600000u | fRd(MI,0) | fRs1(MI,1) | fRs2(MI,2);

  // ── Class 5 unary (rd,rs1 at ops 0,1) and 2-address in-place (rd=rs1) ─────
  // COPY / MEMGET* have independent rd,rs at ops 0,1; the in-place unary ops
  // are tied ($rd=$rs1), so operand 0 and operand 1 are the same register.
  case KlaussCPU::COPY_R:    return 0x54000000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::NEGR:      return 0x54480000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::NOTR:      return 0x54880000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::ABSR:      return 0x54C80000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::SEXTB:     return 0x55080000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::SEXTH:     return 0x55180000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::SEXTW:     return 0x55200000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::ZEXTB:     return 0x55480000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::ZEXTH:     return 0x55580000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::ZEXTW:     return 0x55600000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::BSWAP_R:   return 0x55800000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::BITREV:    return 0x55C00000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::POPCNT:    return 0x56080000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::CLZ:       return 0x56400000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::CTZ:       return 0x56800000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::INCR:      return 0x57880000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::DECR:      return 0x57C80000u | fRd(MI,0) | fRs1(MI,1);

  // SETFR writes only rd (rs1 field must stay 0 even though the .td ties it).
  case KlaussCPU::SETFR:     return 0x57000000u | fRd(MI,0);

  // ── Class 4 shift/rotate-by-1 and by-immediate-N (1 word in v2) ───────────
  // Fixed-N (=1) forms: rd,rs1 (2-address, tied).
  case KlaussCPU::SHLR1:  return 0x50208000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::SHLAR:  return 0x50208000u | fRd(MI,0) | fRs1(MI,1); // alias of SHLR1
  case KlaussCPU::SHRR1:  return 0x50608000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::SHRAR:  return 0x50A08000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::ROLR1:  return 0x50E0C000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::RORR1:  return 0x5120C000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::ROLCR:  return 0x5160C000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::RORCR:  return 0x51A0C000u | fRd(MI,0) | fRs1(MI,1);

  // Embedded-count forms: N at word0[20:15], rd,rs1 (tied), count at op 2.
  case KlaussCPU::SHLV:   return 0x50204000u | ((getImm32(MI,2) & 0x3F) << 15) | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::SHRV:   return 0x50604000u | ((getImm32(MI,2) & 0x3F) << 15) | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::SHRAV:  return 0x50A04000u | ((getImm32(MI,2) & 0x3F) << 15) | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::ROLV:   return 0x50E04000u | ((getImm32(MI,2) & 0x3F) << 15) | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::RORV:   return 0x51204000u | ((getImm32(MI,2) & 0x3F) << 15) | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::BSET:   return 0x52200000u | ((getImm32(MI,2) & 0x3F) << 15) | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::BCLR:   return 0x52600000u | ((getImm32(MI,2) & 0x3F) << 15) | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::BTGL:   return 0x52A00000u | ((getImm32(MI,2) & 0x3F) << 15) | fRd(MI,0) | fRs1(MI,1);
  // BTST-imm is flag-only: rs1 + embedded N, no rd write (op 0 = rs1, op 1 = N).
  case KlaussCPU::BTST:   return 0x52E00000u | ((getImm32(MI,1) & 0x3F) << 15) | fRs1(MI,0);

  // ── Class 6/7 register-addressed loads / stores (1 word) ──────────────────
  // Loads: rd,rs1(base) at ops 0,1.  Stores: data->rd, addr->rs1 at ops 0,1.
  case KlaussCPU::MEMGET8:    return 0x58000000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::MEMGET16:   return 0x59000000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::MEMGET32:   return 0x5A000000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::MEMREADRR:  return 0x5B000000u | fRd(MI,0) | fRs1(MI,1); // raw 64
  case KlaussCPU::MEMGET64:   return 0x5B100000u | fRd(MI,0) | fRs1(MI,1); // aligned 64
  case KlaussCPU::MEMSET8:    return 0x5C000000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::MEMSET16:   return 0x5D000000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::MEMSET32:   return 0x5E000000u | fRd(MI,0) | fRs1(MI,1);
  case KlaussCPU::MEMSET64RR: return 0x5F000000u | fRd(MI,0) | fRs1(MI,1); // raw 64
  case KlaussCPU::MEMSET64:   return 0x5F100000u | fRd(MI,0) | fRs1(MI,1); // aligned 64

  // Register-offset indexed 64-bit (v2: 1 word; offset register in rs2[3:0]).
  // The .td models the offset as a register *number* immediate (op 2).
  case KlaussCPU::LDIDX64R: return 0x5B600000u | fRd(MI,0) | fRs1(MI,1) | (getImm32(MI,2) & 0xF);
  case KlaussCPU::STIDX64R: return 0x5F600000u | fRd(MI,0) | fRs1(MI,1) | (getImm32(MI,2) & 0xF);

  // ── Class 3 flag-setting compare (rs1,rs2 at ops 0,1) ─────────────────────
  case KlaussCPU::CMPRR_I: return 0x4C000000u | fRs1(MI,0) | fRs2(MI,1);

  // ── Class 9 stack (single register in its designated field) ───────────────
  case KlaussCPU::PUSH_R:  return 0x64000000u | fRs1(MI,0);
  case KlaussCPU::POP_R:   return 0x64800000u | fRd (MI,0);
  case KlaussCPU::GETSP_R: return 0x64C00000u | fRd (MI,0);
  case KlaussCPU::SETSP_R: return 0x65000000u | fRs1(MI,0);
  case KlaussCPU::RET_I:   return 0x65800000u;
  case KlaussCPU::IRET_I:  return 0x65C00000u;

  // ── Class 8 register-target branch / call (target in rs2[3:0]) ────────────
  case KlaussCPU::JMPR_R:  return 0x60800000u | fRs2(MI,0);
  case KlaussCPU::CALLR_R: return 0x62800000u | fRs2(MI,0);

  // ── Class B system (no register/immediate operands) ───────────────────────
  case KlaussCPU::NOP_I:   return 0x6C000000u;
  case KlaussCPU::HALT_I:  return 0x6C010000u;
  case KlaussCPU::WAIT_I:  return 0x6C020000u;

  // ── Class B / C register-source peripherals (source register in rs1[7:4]) ─
  case KlaussCPU::DELAYR:   return 0x6C050000u | fRs1(MI,0);
  case KlaussCPU::LCDDATAR: return 0x71000000u | fRs1(MI,0);

  // ── RETIRED in v2 (LED / 7-seg / RGB): no v2 encoding — these keep their
  //    v1 opcodes purely so hand-written inline asm still assembles.  Their
  //    LEN[31:30]=00 means the v2 CPU traps (ERR_INV_OPCODE) if one executes,
  //    which is the intended fail-fast behaviour.  Codegen never emits them.
  case KlaussCPU::LEDR_R:      return 0x00003000u | getReg(MI,0);
  case KlaussCPU::SEG7_1R_R:   return 0x00003020u | getReg(MI,0);
  case KlaussCPU::SEG7_2R_R:   return 0x00003030u | getReg(MI,0);
  case KlaussCPU::SEG7R_R:     return 0x00003040u | getReg(MI,0);
  case KlaussCPU::SEG7BLANK_I: return 0x00003073u;
  case KlaussCPU::RGB1R:       return 0x00003050u | getReg(MI,0);
  case KlaussCPU::RGB2R:       return 0x00003060u | getReg(MI,0);

  default:
    llvm_unreachable("unhandled 4-byte KlaussCPU instruction in encode32");
  }
}

//===----------------------------------------------------------------------===//
// 8-byte (2-word) instruction dispatch — returns word0<<32 | word1
//===----------------------------------------------------------------------===//

uint64_t KlaussCPUMCCodeEmitter::encode64(
    const MCInst &MI, SmallVectorImpl<MCFixup> &Fixups) const {
  switch (MI.getOpcode()) {

  // ── Class 2 register ← 32-bit immediate / PC-relative address ─────────────
  // SETR (rd, imm-or-symbol) and LEAPC (rd, PC-relative symbol).
  case KlaussCPU::SETR:
    return pack2Sym(0x8BD00000u | fRd(MI,0), MI, 1,
                    KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::LEAPC:
    return pack2Sym(0x8B800000u | fRd(MI,0), MI, 1,
                    KlaussCPU::FK_KlaussCPU_PCREL32, Fixups);

  // Class 2 reg+imm add (rd,rs1 independent): rd = rs1 + sext(imm32).
  case KlaussCPU::ADDI:
    return pack2(0x88300000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2));

  // Class 2 in-place ALU-immediate (rd=rs1, tied): imm at op 2.
  case KlaussCPU::ADDV:   return pack2(0x88200000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2));
  case KlaussCPU::MINUSV: return pack2(0x88600000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2));
  case KlaussCPU::ANDV:   return pack2(0x89000000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2));
  case KlaussCPU::ORV:    return pack2(0x89400000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2));
  case KlaussCPU::XORV:   return pack2(0x89800000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2));

  // Class A in-place mul/div/mod-immediate (rd=rs1, tied): imm at op 2.
  case KlaussCPU::MULV:   return pack2(0xA8800000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2));
  case KlaussCPU::DIVV:   return pack2(0xA9800000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2));
  case KlaussCPU::MODV:   return pack2(0xAA800000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2));

  // Class 4 bit-field extract/deposit (2 words; params packed in imm32).
  case KlaussCPU::BEXTR:  return pack2(0x93000000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2));
  case KlaussCPU::BDEP:   return pack2(0x93400000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2));

  // Class 3 compare register to immediate — flags only (rs1 + imm32).
  case KlaussCPU::CMPRV_I: return pack2(0x8C100000u | fRs1(MI,0), getImm32(MI,1));

  // ── Class 6 indexed loads: rd,base at ops 0,1; offset32 at op 2 ───────────
  case KlaussCPU::LDIDX8:    return pack2(0x98200000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2));
  case KlaussCPU::LDIDX8_S:  return pack2(0x98A00000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2));
  case KlaussCPU::LDIDX16:   return pack2(0x99200000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2));
  case KlaussCPU::LDIDX16_S: return pack2(0x99A00000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2));
  case KlaussCPU::LDIDX32:   return pack2(0x9A200000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2));
  case KlaussCPU::LDIDX64:   return pack2(0x9B300000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2)); // aligned (v2 LDIDX64A)
  case KlaussCPU::LDIDX64U:  return pack2(0x9B200000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2)); // raw (v2 LDIDX64)

  // ── Class 7 indexed stores: data->rd, base->rs1 at ops 0,1; offset32 op 2 ─
  case KlaussCPU::STIDX8:   return pack2(0x9C200000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2));
  case KlaussCPU::STIDX16:  return pack2(0x9D200000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2));
  case KlaussCPU::STIDX32:  return pack2(0x9E200000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2));
  case KlaussCPU::STIDX64:  return pack2(0x9F300000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2)); // aligned (v2 STIDX64A)
  case KlaussCPU::STIDX64U: return pack2(0x9F200000u | fRd(MI,0) | fRs1(MI,1), getImm32(MI,2)); // raw (v2 STIDX64)

  // ── Class 6/7 absolute-address memory (symbol-capable) ────────────────────
  case KlaussCPU::MEMREADR: // rd = mem64[addr32]
    return pack2Sym(0x9B400000u | fRd(MI,0), MI, 1,
                    KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::MEMSETR:  // mem64[addr32] = reg[rd]  (.td op0 = data, op1 = addr)
    return pack2Sym(0x9F400000u | fRd(MI,0), MI, 1,
                    KlaussCPU::FK_KlaussCPU_ABS32, Fixups);

  // ── Class 8 absolute-target branches (target32 at op 0, ABS32 fixup) ──────
  case KlaussCPU::JMP:    return pack2Sym(0xA0000000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::JMPZ:   return pack2Sym(0xA0080000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::JMPNZ:  return pack2Sym(0xA00C0000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::JMPC:   return pack2Sym(0xA0100000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::JMPNC:  return pack2Sym(0xA0140000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::JMPO:   return pack2Sym(0xA0180000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::JMPNO:  return pack2Sym(0xA01C0000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::JMPS:   return pack2Sym(0xA0200000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::JMPNS:  return pack2Sym(0xA0240000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::JMPLT:  return pack2Sym(0xA0280000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::JMPGE:  return pack2Sym(0xA02C0000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::JMPLE:  return pack2Sym(0xA0300000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::JMPGT:  return pack2Sym(0xA0340000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::JMPULT: return pack2Sym(0xA0380000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::JMPUGE: return pack2Sym(0xA03C0000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::JMPULE: return pack2Sym(0xA0400000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::JMPUGT: return pack2Sym(0xA0440000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::JMPE:   return pack2Sym(0xA0480000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::JMPNE:  return pack2Sym(0xA04C0000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);

  // ── Class 8 absolute-target calls (ABS32 fixup) ───────────────────────────
  case KlaussCPU::CALL_I:  return pack2Sym(0xA2000000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::CALLZ:   return pack2Sym(0xA2080000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::CALLNZ:  return pack2Sym(0xA20C0000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::CALLC:   return pack2Sym(0xA2100000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::CALLNC:  return pack2Sym(0xA2140000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::CALLO:   return pack2Sym(0xA2180000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::CALLNO:  return pack2Sym(0xA21C0000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::CALLE:   return pack2Sym(0xA2480000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);
  case KlaussCPU::CALLNE:  return pack2Sym(0xA24C0000u, MI, 0, KlaussCPU::FK_KlaussCPU_ABS32, Fixups);

  // ── Class 8 PC-relative branches / call (PCREL32 fixup) ───────────────────
  case KlaussCPU::JMPREL:    return pack2Sym(0xA1000000u, MI, 0, KlaussCPU::FK_KlaussCPU_PCREL32, Fixups);
  case KlaussCPU::JMPZREL:   return pack2Sym(0xA1080000u, MI, 0, KlaussCPU::FK_KlaussCPU_PCREL32, Fixups);
  case KlaussCPU::JMPNZREL:  return pack2Sym(0xA10C0000u, MI, 0, KlaussCPU::FK_KlaussCPU_PCREL32, Fixups);
  case KlaussCPU::JMPCREL:   return pack2Sym(0xA1100000u, MI, 0, KlaussCPU::FK_KlaussCPU_PCREL32, Fixups);
  case KlaussCPU::JMPNCREL:  return pack2Sym(0xA1140000u, MI, 0, KlaussCPU::FK_KlaussCPU_PCREL32, Fixups);
  case KlaussCPU::JMPSREL:   return pack2Sym(0xA1200000u, MI, 0, KlaussCPU::FK_KlaussCPU_PCREL32, Fixups);
  case KlaussCPU::JMPNSREL:  return pack2Sym(0xA1240000u, MI, 0, KlaussCPU::FK_KlaussCPU_PCREL32, Fixups);
  case KlaussCPU::JMPLTREL:  return pack2Sym(0xA1280000u, MI, 0, KlaussCPU::FK_KlaussCPU_PCREL32, Fixups);
  case KlaussCPU::JMPGEREL:  return pack2Sym(0xA12C0000u, MI, 0, KlaussCPU::FK_KlaussCPU_PCREL32, Fixups);
  case KlaussCPU::JMPLEREL:  return pack2Sym(0xA1300000u, MI, 0, KlaussCPU::FK_KlaussCPU_PCREL32, Fixups);
  case KlaussCPU::JMPGTREL:  return pack2Sym(0xA1340000u, MI, 0, KlaussCPU::FK_KlaussCPU_PCREL32, Fixups);
  case KlaussCPU::JMPULTREL: return pack2Sym(0xA1380000u, MI, 0, KlaussCPU::FK_KlaussCPU_PCREL32, Fixups);
  case KlaussCPU::JMPUGEREL: return pack2Sym(0xA13C0000u, MI, 0, KlaussCPU::FK_KlaussCPU_PCREL32, Fixups);
  case KlaussCPU::JMPULEREL: return pack2Sym(0xA1400000u, MI, 0, KlaussCPU::FK_KlaussCPU_PCREL32, Fixups);
  case KlaussCPU::JMPUGTREL: return pack2Sym(0xA1440000u, MI, 0, KlaussCPU::FK_KlaussCPU_PCREL32, Fixups);
  case KlaussCPU::JMPEREL:   return pack2Sym(0xA1480000u, MI, 0, KlaussCPU::FK_KlaussCPU_PCREL32, Fixups);
  case KlaussCPU::JMPNEREL:  return pack2Sym(0xA14C0000u, MI, 0, KlaussCPU::FK_KlaussCPU_PCREL32, Fixups);
  case KlaussCPU::CALLREL:   return pack2Sym(0xA3000000u, MI, 0, KlaussCPU::FK_KlaussCPU_PCREL32, Fixups);

  // ── Class 9 stack immediate ops (imm32 at op 0) ───────────────────────────
  case KlaussCPU::ADDSP_I: return pack2(0xA5400000u, getImm32(MI,0)); // sext
  case KlaussCPU::PUSHV:   return pack2(0xA4400000u, getImm32(MI,0)); // zext

  // ── Class B / C immediate peripherals (imm32 at op 0) ─────────────────────
  case KlaussCPU::DELAYV:   return pack2(0xAC050000u, getImm32(MI,0));
  case KlaussCPU::LCDCMDV:  return pack2(0xB0000000u, getImm32(MI,0));
  case KlaussCPU::LCDDATAV: return pack2(0xB1000000u, getImm32(MI,0));
  case KlaussCPU::LCD:      return pack2(0xB2000000u, getImm32(MI,0)); // v2 LCDRST

  // ── RETIRED in v2 (LED / RGB immediate): v1 opcodes retained (see above) ──
  case KlaussCPU::LEDV:  return pack2(0x00003070u, getImm32(MI,0));
  case KlaussCPU::RGB1V: return pack2(0x00003074u, getImm32(MI,0));
  case KlaussCPU::RGB2V: return pack2(0x00003075u, getImm32(MI,0));

  default:
    llvm_unreachable("unhandled 8-byte KlaussCPU instruction in encode64");
  }
}

//===----------------------------------------------------------------------===//
// Top-level dispatch
//===----------------------------------------------------------------------===//

void KlaussCPUMCCodeEmitter::encodeInstruction(
    const MCInst &MI, SmallVectorImpl<char> &CB,
    SmallVectorImpl<MCFixup> &Fixups, const MCSubtargetInfo & /*STI*/) const {

  unsigned Opcode = MI.getOpcode();

  // SETR64: 12-byte, 3-word (v2 class 2, LEN=11).  Template 0xCBC00000.
  //   Word0 = template | rd<<8,  Word1 = lo32,  Word2 = hi32.
  if (Opcode == KlaussCPU::SETR64) {
    emitLE32(0xCBC00000u | fRd(MI, 0), CB);
    emitLE32(getImm32(MI, 1), CB); // lo
    emitLE32(getImm32(MI, 2), CB); // hi
    return;
  }

  // PUSHV64: 12-byte push of a 64-bit immediate (v2 class 9, LEN=11).
  //   Word0 = template 0xE4400000,  Word1 = lo32,  Word2 = hi32.
  if (Opcode == KlaussCPU::PUSHV64) {
    emitLE32(0xE4400000u, CB);
    emitLE32(getImm32(MI, 0), CB); // lo
    emitLE32(getImm32(MI, 1), CB); // hi
    return;
  }

  unsigned Size = MCII.get(Opcode).getSize();
  if (Size == 4) {
    emitLE32(encode32(MI), CB);
  } else if (Size == 8) {
    uint64_t Bits = encode64(MI, Fixups);
    emitLE32(static_cast<uint32_t>(Bits >> 32), CB); // word0 (opcode + regs)
    emitLE32(static_cast<uint32_t>(Bits),        CB); // word1 (imm / target)
  } else {
    llvm_unreachable("unexpected KlaussCPU instruction size in encodeInstruction");
  }
}

//===----------------------------------------------------------------------===//
// Factory
//===----------------------------------------------------------------------===//

namespace llvm {
MCCodeEmitter *createKlaussCPUMCCodeEmitter(const MCInstrInfo &MCII,
                                             MCContext &Ctx) {
  return new KlaussCPUMCCodeEmitter(MCII, Ctx);
}
} // namespace llvm
