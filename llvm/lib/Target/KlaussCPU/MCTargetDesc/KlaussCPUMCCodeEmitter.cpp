//===-- KlaussCPUMCCodeEmitter.cpp - KlaussCPU instruction encoder ---------===//
//
// Converts MCInst to raw bytes for ELF object output.
//
// All instruction words are written little-endian (least-significant byte first),
// matching the KlaussCPU LE physical memory.  The CPU instruction-fetch unit
// reads each 32-bit word with the byte at the lowest address in bits[7:0].
//
// Instruction word layouts (tablegen bit numbering; bit N = 2^N):
//
//  KCInst32 (4 B, 1 word):
//    RRR    [31:12]=op20  [11:8]=rd   [7:4]=rs1  [3:0]=rs2
//    RR1    [31:8] =op24  [7:4] =rd   [3:0]=rs
//    RR0    [31:8] =op24  [7:4] =rs1  [3:0]=rs2
//    RRst   [31:8] =op24  [7:4] =rs_data  [3:0]=rs_addr
//    R      [31:4] =op28  [3:0] =reg
//    I0     [31:0] =exact_opcode32
//
//  KCInst64 (8 B, 2 words — high word emitted first):
//    RV_ld    [63:36]=op28  [35:32]=rd   [31:0]=imm32
//    RV_2addr [63:36]=op28  [35:32]=rd   [31:0]=imm32  ($rd=$rs tie)
//    RV_cmp   [63:36]=op28  [35:32]=rs   [31:0]=imm32
//    RRV      [63:40]=op24  [39:36]=rd   [35:32]=base  [31:0]=offset32
//    Vimm     [63:32]=exact_op32          [31:0]=target/imm
//
//  KCInst96 (12 B, 3 words — SETR64 only):
//    Word0: [31:8]=op24  [7:4]=rd  [3:0]=0
//    Word1: lo32
//    Word2: hi32
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

  // ── Per-format encoding helpers ───────────────────────────────────────────

  // RRR: (op20<<12) | (rd<<8) | (rs1<<4) | rs2   MCInst:[0]=rd [1]=rs1 [2]=rs2
  uint32_t fmtRRR(uint32_t op20, const MCInst &MI) const {
    return (op20 << 12) | (getReg(MI,0) << 8) | (getReg(MI,1) << 4) | getReg(MI,2);
  }

  // RR1: (op24<<8) | (rd<<4) | rs   MCInst:[0]=rd [1]=rs
  uint32_t fmtRR1(uint32_t op24, const MCInst &MI) const {
    return (op24 << 8) | (getReg(MI,0) << 4) | getReg(MI,1);
  }

  // RR0 / RRst: (op24<<8) | (r0<<4) | r1   MCInst:[0]=r0 [1]=r1
  uint32_t fmtRR2(uint32_t op24, const MCInst &MI) const {
    return (op24 << 8) | (getReg(MI,0) << 4) | getReg(MI,1);
  }

  // R_inplace, R_in, R_out: (op28<<4) | reg(OpNo)
  uint32_t fmtR(uint32_t op28, const MCInst &MI, unsigned OpNo) const {
    return (op28 << 4) | getReg(MI, OpNo);
  }

  // RV_ld: ((op28<<36) | (rd<<32) | imm)   MCInst:[0]=rd [1]=imm
  uint64_t fmtRV_ld(uint64_t op28, const MCInst &MI) const {
    return (op28 << 36)
         | (static_cast<uint64_t>(getReg(MI, 0)) << 32)
         | getImm32(MI, 1);
  }

  // RV_2addr: ((op28<<36) | (rd<<32) | imm)   MCInst:[0]=rd [1]=rs(tied) [2]=imm
  uint64_t fmtRV_2addr(uint64_t op28, const MCInst &MI) const {
    return (op28 << 36)
         | (static_cast<uint64_t>(getReg(MI, 0)) << 32)
         | getImm32(MI, 2);
  }

  // RV_cmp: ((op28<<36) | (rs<<32) | imm)   MCInst:[0]=rs [1]=imm
  uint64_t fmtRV_cmp(uint64_t op28, const MCInst &MI) const {
    return (op28 << 36)
         | (static_cast<uint64_t>(getReg(MI, 0)) << 32)
         | getImm32(MI, 1);
  }

  // RRV_ld: ((op24<<40) | (rd<<36) | (base<<32) | off)
  //   MCInst:[0]=rd [1]=base [2]=off
  uint64_t fmtRRV_ld(uint64_t op24, const MCInst &MI) const {
    return (op24 << 40)
         | (static_cast<uint64_t>(getReg(MI, 0)) << 36)
         | (static_cast<uint64_t>(getReg(MI, 1)) << 32)
         | getImm32(MI, 2);
  }

  // RRV_st: ((op24<<40) | (rs_data<<36) | (base<<32) | off)
  //   MCInst:[0]=rs_data [1]=base [2]=off
  uint64_t fmtRRV_st(uint64_t op24, const MCInst &MI) const {
    return (op24 << 40)
         | (static_cast<uint64_t>(getReg(MI, 0)) << 36)
         | (static_cast<uint64_t>(getReg(MI, 1)) << 32)
         | getImm32(MI, 2);
  }

  // Vimm with plain immediate: ((op32<<32) | imm)   MCInst:[0]=imm
  uint64_t fmtVimm(uint64_t op32, const MCInst &MI) const {
    return (op32 << 32) | getImm32(MI, 0);
  }

  // Vbr / Vcall: ((op32<<32) | target32).
  // target is always an MCExpr (basic-block or global symbol); adds a fixup
  // at byte offset 4 within the 8-byte instruction (word 1).
  uint64_t fmtVbr(uint64_t op32, const MCInst &MI,
                   SmallVectorImpl<MCFixup> &Fixups) const {
    const MCOperand &MO = MI.getOperand(0);
    uint32_t Target = 0;
    if (MO.isImm()) {
      Target = static_cast<uint32_t>(MO.getImm());
    } else {
      assert(MO.isExpr() && "branch/call target must be imm or expr");
      Fixups.push_back(MCFixup::create(
          4, MO.getExpr(), MCFixupKind(KlaussCPU::FK_KlaussCPU_ABS32)));
    }
    return (op32 << 32) | Target;
  }

  uint32_t encode32(const MCInst &MI) const;
  uint64_t encode64(const MCInst &MI, SmallVectorImpl<MCFixup> &Fixups) const;
};

} // anonymous namespace

//===----------------------------------------------------------------------===//
// 4-byte instruction dispatch
//===----------------------------------------------------------------------===//

uint32_t KlaussCPUMCCodeEmitter::encode32(const MCInst &MI) const {
  switch (MI.getOpcode()) {

  // ── RRR format ───────────────────────────────────────────────────────────
  case KlaussCPU::ADDR:    return fmtRRR(0x00010, MI);
  case KlaussCPU::SUBR:    return fmtRRR(0x00020, MI);
  case KlaussCPU::ANDR:    return fmtRRR(0x00030, MI);
  case KlaussCPU::ORR:     return fmtRRR(0x00040, MI);
  case KlaussCPU::XORR:    return fmtRRR(0x00050, MI);
  case KlaussCPU::MULR:    return fmtRRR(0x00100, MI);
  case KlaussCPU::MULUR:   return fmtRRR(0x00110, MI);
  case KlaussCPU::MULHR:   return fmtRRR(0x00120, MI);
  case KlaussCPU::MULHUR:  return fmtRRR(0x00130, MI);
  case KlaussCPU::DIVR:    return fmtRRR(0x00140, MI);
  case KlaussCPU::DIVUR:   return fmtRRR(0x00150, MI);
  case KlaussCPU::MODR:    return fmtRRR(0x00160, MI);
  case KlaussCPU::MODUR:   return fmtRRR(0x00170, MI);
  case KlaussCPU::SHLR:    return fmtRRR(0x00200, MI);
  case KlaussCPU::SHRR:    return fmtRRR(0x00210, MI);
  case KlaussCPU::SARR:    return fmtRRR(0x00220, MI);
  case KlaussCPU::ROLR:    return fmtRRR(0x00230, MI);
  case KlaussCPU::RORR_R:  return fmtRRR(0x00240, MI);
  case KlaussCPU::CMPEQR:  return fmtRRR(0x00300, MI);
  case KlaussCPU::CMPNER:  return fmtRRR(0x00310, MI);
  case KlaussCPU::CMPLTR:  return fmtRRR(0x00320, MI);
  case KlaussCPU::CMPLER:  return fmtRRR(0x00330, MI);
  case KlaussCPU::CMPGTR:  return fmtRRR(0x00340, MI);
  case KlaussCPU::CMPGER:  return fmtRRR(0x00350, MI);
  case KlaussCPU::CMPULTR: return fmtRRR(0x00360, MI);
  case KlaussCPU::CMPULER: return fmtRRR(0x00370, MI);
  case KlaussCPU::CMPUGTR: return fmtRRR(0x00380, MI);
  case KlaussCPU::CMPUGER: return fmtRRR(0x00390, MI);
  case KlaussCPU::MINR:    return fmtRRR(0x00400, MI);
  case KlaussCPU::MAXR:    return fmtRRR(0x00410, MI);
  case KlaussCPU::MINUR:   return fmtRRR(0x00420, MI);
  case KlaussCPU::MAXUR:   return fmtRRR(0x00430, MI);

  // ── RR1 (rd, rs) ─────────────────────────────────────────────────────────
  case KlaussCPU::COPY_R:   return fmtRR1(0x000001, MI);
  case KlaussCPU::MEMGET64: return fmtRR1(0x00007B, MI);
  case KlaussCPU::MEMGET8:  return fmtRR1(0x000075, MI);
  case KlaussCPU::MEMGET16: return fmtRR1(0x000077, MI);
  case KlaussCPU::MEMGET32: return fmtRR1(0x000079, MI);

  // ── RR0 (rs1, rs2, flags only) ───────────────────────────────────────────
  case KlaussCPU::CMPRR_I:  return fmtRR2(0x000005, MI);

  // ── RRst (rs_data, rs_addr) ──────────────────────────────────────────────
  case KlaussCPU::MEMSET64: return fmtRR2(0x00007A, MI);
  case KlaussCPU::MEMSET8:  return fmtRR2(0x000074, MI);
  case KlaussCPU::MEMSET16: return fmtRR2(0x000076, MI);
  case KlaussCPU::MEMSET32: return fmtRR2(0x000078, MI);

  // ── R_inplace (def at operand 0; use tied = same reg) ────────────────────
  case KlaussCPU::INCR:    return fmtR(0x0000084, MI, 0);
  case KlaussCPU::DECR:    return fmtR(0x0000085, MI, 0);
  case KlaussCPU::NEGR:    return fmtR(0x000008A, MI, 0);
  case KlaussCPU::ABSR:    return fmtR(0x000008B, MI, 0);
  case KlaussCPU::SHLR1:   return fmtR(0x000008D, MI, 0);
  case KlaussCPU::SHRR1:   return fmtR(0x000008E, MI, 0);
  case KlaussCPU::NOTR:    return fmtR(0x0000098, MI, 0);
  case KlaussCPU::POPCNT:  return fmtR(0x00000A8, MI, 0);
  case KlaussCPU::CLZ:     return fmtR(0x00000A9, MI, 0);
  case KlaussCPU::CTZ:     return fmtR(0x00000AA, MI, 0);
  case KlaussCPU::BITREV:  return fmtR(0x00000AB, MI, 0);
  case KlaussCPU::BSWAP_R: return fmtR(0x0000097, MI, 0);
  case KlaussCPU::SEXTB:   return fmtR(0x000008C, MI, 0);
  case KlaussCPU::SEXTH:   return fmtR(0x0000094, MI, 0);
  case KlaussCPU::ZEXTB:   return fmtR(0x0000095, MI, 0);
  case KlaussCPU::ZEXTH:   return fmtR(0x0000096, MI, 0);
  case KlaussCPU::SEXTW:   return fmtR(0x00000F0, MI, 0);
  case KlaussCPU::ZEXTW:   return fmtR(0x00000F1, MI, 0);

  // ── R_in (input reg at operand 0) ────────────────────────────────────────
  case KlaussCPU::PUSH_R:  return fmtR(0x0000400, MI, 0);
  case KlaussCPU::SETSP_R: return fmtR(0x0000404, MI, 0);
  case KlaussCPU::JMPR_R:  return fmtR(0x0000102, MI, 0);
  case KlaussCPU::CALLR_R: return fmtR(0x0000407, MI, 0);

  // ── R_out (output reg at operand 0) ──────────────────────────────────────
  case KlaussCPU::POP_R:   return fmtR(0x0000401, MI, 0);
  case KlaussCPU::GETSP_R: return fmtR(0x0000403, MI, 0);

  // ── UART / I-O — R_in (operand 0 = source register) ─────────────────────
  case KlaussCPU::TXR_R:        return fmtR(0x0000501, MI, 0);
  case KlaussCPU::TXMEMR_R:     return fmtR(0x0000502, MI, 0);
  case KlaussCPU::TXCHARMEMR_R: return fmtR(0x0000503, MI, 0);
  case KlaussCPU::TXSTRMEMR_R:  return fmtR(0x0000504, MI, 0);

  // ── UART / I-O — R_out (operand 0 = destination register) ───────────────
  case KlaussCPU::RXRB_R:       return fmtR(0x0000505, MI, 0);
  case KlaussCPU::RXRNB_R:      return fmtR(0x0000506, MI, 0);

  // ── LEDs / 7-seg ─────────────────────────────────────────────────────────
  case KlaussCPU::LEDR_R:       return fmtR(0x0000300, MI, 0);
  case KlaussCPU::SEG7_1R_R:    return fmtR(0x0000302, MI, 0);
  case KlaussCPU::SEG7_2R_R:    return fmtR(0x0000303, MI, 0);
  case KlaussCPU::SEG7R_R:      return fmtR(0x0000304, MI, 0);

  // ── I0 (fixed opcode, no register/immediate operands) ────────────────────
  case KlaussCPU::NEWLINE_I:    return 0x00005001;
  case KlaussCPU::SEG7BLANK_I:  return 0x00003073;
  case KlaussCPU::RET_I:     return 0x00001012;
  case KlaussCPU::NOP_I:     return 0x0000F010;
  case KlaussCPU::HALT_I:    return 0x0000F011;
  case KlaussCPU::IRET_I:    return 0x00006011;

  default:
    llvm_unreachable("unhandled 4-byte KlaussCPU instruction in encode32");
  }
}

//===----------------------------------------------------------------------===//
// 8-byte instruction dispatch
//===----------------------------------------------------------------------===//

uint64_t KlaussCPUMCCodeEmitter::encode64(
    const MCInst &MI, SmallVectorImpl<MCFixup> &Fixups) const {
  switch (MI.getOpcode()) {

  // ── RV_ld (rd, imm32-or-symbol) ─────────────────────────────────────────
  // SETR is used both for plain immediates and for global-address loads.
  // When the operand is an MCExpr (symbol reference), emit a fixup at word 1
  // (byte offset 4) so the linker fills in the 32-bit absolute address.
  case KlaussCPU::SETR: {
    uint64_t W0 = (static_cast<uint64_t>(0x0000080u) << 36)
                | (static_cast<uint64_t>(getReg(MI, 0)) << 32);
    const MCOperand &MO = MI.getOperand(1);
    if (MO.isImm())
      return W0 | static_cast<uint32_t>(MO.getImm());
    assert(MO.isExpr() && "SETR: operand 1 must be imm or expr");
    Fixups.push_back(MCFixup::create(
        4, MO.getExpr(), MCFixupKind(KlaussCPU::FK_KlaussCPU_ABS32)));
    return W0;
  }

  // ── RV_2addr (rd=rs tied, imm32 at operand 2) ────────────────────────────
  case KlaussCPU::ADDV:    return fmtRV_2addr(0x0000081, MI);
  case KlaussCPU::MINUSV:  return fmtRV_2addr(0x0000082, MI);
  case KlaussCPU::ANDV:    return fmtRV_2addr(0x0000086, MI);
  case KlaussCPU::ORV:     return fmtRV_2addr(0x0000087, MI);
  case KlaussCPU::XORV:    return fmtRV_2addr(0x0000088, MI);
  case KlaussCPU::SHLV:    return fmtRV_2addr(0x0000091, MI);
  case KlaussCPU::SHRV:    return fmtRV_2addr(0x0000092, MI);
  case KlaussCPU::SHRAV:   return fmtRV_2addr(0x0000093, MI);
  case KlaussCPU::MULV:    return fmtRV_2addr(0x00000B8, MI);
  case KlaussCPU::DIVV:    return fmtRV_2addr(0x00000B9, MI);
  case KlaussCPU::MODV:    return fmtRV_2addr(0x00000BA, MI);

  // ── RV_cmp (rs, imm32) ───────────────────────────────────────────────────
  case KlaussCPU::CMPRV_I: return fmtRV_cmp(0x0000083, MI);

  // ── RRV ALU (rd, rs, imm32) ──────────────────────────────────────────────
  case KlaussCPU::ADDI: return fmtRRV_ld(0x000002, MI);

  // ── RRV loads (rd, base, offset32) ───────────────────────────────────────
  case KlaussCPU::LDIDX64:   return fmtRRV_ld(0x0000FC, MI);
  case KlaussCPU::LDIDX32:   return fmtRRV_ld(0x0000C0, MI);
  case KlaussCPU::LDIDX16:   return fmtRRV_ld(0x0000C2, MI);
  case KlaussCPU::LDIDX8:    return fmtRRV_ld(0x0000C4, MI);
  case KlaussCPU::LDIDX8_S:  return fmtRRV_ld(0x0000C6, MI);
  case KlaussCPU::LDIDX16_S: return fmtRRV_ld(0x0000C7, MI);

  // ── RRV stores (rs_data, base, offset32) ─────────────────────────────────
  case KlaussCPU::STIDX64: return fmtRRV_st(0x0000FD, MI);
  case KlaussCPU::STIDX32: return fmtRRV_st(0x0000C1, MI);
  case KlaussCPU::STIDX16: return fmtRRV_st(0x0000C3, MI);
  case KlaussCPU::STIDX8:  return fmtRRV_st(0x0000C5, MI);

  // ── Vbr unconditional ────────────────────────────────────────────────────
  case KlaussCPU::JMP:     return fmtVbr(0x00001000, MI, Fixups);

  // ── Vbr conditional ──────────────────────────────────────────────────────
  case KlaussCPU::JMPZ:    return fmtVbr(0x00001001, MI, Fixups);
  case KlaussCPU::JMPNZ:   return fmtVbr(0x00001002, MI, Fixups);
  case KlaussCPU::JMPE:    return fmtVbr(0x00001003, MI, Fixups);
  case KlaussCPU::JMPNE:   return fmtVbr(0x00001004, MI, Fixups);
  case KlaussCPU::JMPC:    return fmtVbr(0x00001005, MI, Fixups);
  case KlaussCPU::JMPNC:   return fmtVbr(0x00001006, MI, Fixups);
  case KlaussCPU::JMPS:    return fmtVbr(0x00001013, MI, Fixups);
  case KlaussCPU::JMPNS:   return fmtVbr(0x00001014, MI, Fixups);
  case KlaussCPU::JMPLT:   return fmtVbr(0x00001015, MI, Fixups);
  case KlaussCPU::JMPLE:   return fmtVbr(0x00001016, MI, Fixups);
  case KlaussCPU::JMPGT:   return fmtVbr(0x00001017, MI, Fixups);
  case KlaussCPU::JMPGE:   return fmtVbr(0x00001018, MI, Fixups);
  case KlaussCPU::JMPULT:  return fmtVbr(0x00001019, MI, Fixups);
  case KlaussCPU::JMPULE:  return fmtVbr(0x0000101A, MI, Fixups);
  case KlaussCPU::JMPUGT:  return fmtVbr(0x0000101B, MI, Fixups);
  case KlaussCPU::JMPUGE:  return fmtVbr(0x0000101C, MI, Fixups);

  // ── Vcall ─────────────────────────────────────────────────────────────────
  case KlaussCPU::CALL_I:  return fmtVbr(0x00001009, MI, Fixups);

  // ── Vsp (ADDSP / DELAYV: fixed op32, signed imm32 at operand 0) ──────────
  case KlaussCPU::ADDSP_I:  return fmtVimm(0x00004050, MI);

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

  // SETR64: 12-byte instruction (three big-endian 32-bit words).
  // Word0 uses R format: op28 in bits[31:4], rd in bits[3:0].
  // opcode = 0x00000FE → Word0 = (0x00000FE << 4) | rd = 0x00000FEx
  // Word1 = lo32, Word2 = hi32.
  if (Opcode == KlaussCPU::SETR64) {
    unsigned Rd = getReg(MI, 0);
    uint32_t Lo = getImm32(MI, 1);
    uint32_t Hi = getImm32(MI, 2);
    emitLE32((static_cast<uint32_t>(0x00000FEu) << 4) | Rd, CB);
    emitLE32(Lo, CB);
    emitLE32(Hi, CB);
    return;
  }

  unsigned Size = MCII.get(Opcode).getSize();
  if (Size == 4) {
    emitLE32(encode32(MI), CB);
  } else if (Size == 8) {
    uint64_t Bits = encode64(MI, Fixups);
    emitLE32(static_cast<uint32_t>(Bits >> 32), CB); // high word (opcode+regs)
    emitLE32(static_cast<uint32_t>(Bits),        CB); // low word (imm/target)
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
