//===-- KlaussCPUOptExtensions.cpp - Remove redundant extensions ----------===//
//
// i8/i16/i32 values live in 64-bit registers, and instruction selection
// extends them with ZEXTB/ZEXTH/ZEXTW or SEXTB/SEXTH/SEXTW where it cannot
// see that the extension is already there or is never looked at.  Within a
// block the DAG's known bits catch most of these; across blocks it sees only
// what FunctionLoweringInfo records for each exported register, so an
// extension of a PHI, or of a value computed in another block, often
// survives.  For example a loop over a u32 index loaded from memory
// (`idx = (u32)b[idx]`) zero-extends the PHI of the index each iteration
// although every value reaching it was loaded with LDIDX32.
//
// This pass (SSA, before register allocation, as RISC-V's RISCVOptWInstrs)
// deletes an extension of N bits, 8, 16 or 32, when
//
//   1. its source is already extended from at most N bits, following COPY,
//      PHI and the bitwise / min / max ops whose result is one of, or a
//      bitwise combination of, their operands, to the instructions that
//      extend: the extending loads (LDIDXn zero-extends, LDIDXn_S
//      sign-extends: pipeline_core.sv mem_ldval), the extensions themselves,
//      the 32-bit W ops (their result is the sign extension of its low 32
//      bits: pipeline_core.sv dec_t.w32, MEM writeback), boolean compares,
//      CLZ/CTZ/POPCNT, BEXTR (a field of at most 31 bits), and constants
//      that fit; or
//   2. everything that reads its result reads only the low N bits: the W
//      ops and W compares, stores of N bits or fewer (their data operand),
//      narrower extensions, a shift's amount (6 bits), or an add, subtract,
//      multiply, logic op or left shift whose own result is read only in its
//      low N bits (and whose FLAGS are dead), followed through COPY and PHI;
//      or
//   3. its source is a PHI whose other readers are the same extension or
//      read only the low N bits, and each incoming value is extended already
//      or has the same extension in its predecessor block: that extension
//      becomes the incoming value, and then rule 1 applies.
//   4. (SEXTW only) its source is an add, subtract or multiply (ADDR,
//      SUBR, MULR, ADDI, ADDV, MINUSV, INCR, DECR, with dead FLAGS; not an
//      ADDI of a frame index, which eliminateFrameIndex folds) whose
//      every reader reads only its low 32 bits, this SEXTW included: that
//      op becomes its W form (ADDW, SUBW, MULW, ADDIW), whose result is the
//      sign extension of the same low 32 bits, and then rule 1 applies.
//      This is what lets a branch on a 32-bit value compare all 64 bits and
//      fuse into an FBR (KlaussCPUISelDAGToDAG tryW32Compare): a loop's
//      `n++ ; n < max` becomes ADDIW + FBR instead of INCR + CMPRRW + JMPcc.
//      -klausscpu-w32-alu=false turns this rule off with the W-op patterns.
//
// The extension's result is then replaced by its source.  Every decision is
// made on the current code, so removing one extension can only keep another
// from being removed, never make a removal wrong.
//
// Rules 2, 3 and 4 change values above bit N that no instruction reads, but a
// debug value may: the variable can be wider than the bits its code uses.  A
// debug value of the removed extension's result describes the source with
// the extension in its DIExpression (rule 2); one of any other register whose
// high bits change (the readers the rule followed, the PHI of rule 3) is set
// undef unless it describes a variable of at most N bits.
//
// -mllvm -klausscpu-opt-extensions=false disables it.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUInstrInfo.h"
#include "KlaussCPUSubtarget.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

// The pass name must differ from the option below (see KlaussCPUFuseCmpBr).
#define DEBUG_TYPE "klausscpu-opt-ext"
#define PASS_NAME "KlaussCPU redundant extension removal"

STATISTIC(NumExtendedSource, "Extensions of an already extended value removed");
STATISTIC(NumNarrowUsers, "Extensions read only in their low bits removed");
STATISTIC(NumPHIInputs, "Extensions of a PHI removed by extending its inputs");
STATISTIC(NumWOps, "SEXTWs removed by making their source a W op");

namespace llvm {
void initializeKlaussCPUOptExtensionsPass(PassRegistry &);
FunctionPass *createKlaussCPUOptExtensionsPass();
} // namespace llvm

static cl::opt<bool> EnableOptExtensions(
    "klausscpu-opt-extensions", cl::Hidden, cl::init(true),
    cl::desc("Remove sign and zero extensions whose source is already "
             "extended or whose users read only the low bits"));

// -klausscpu-w32-alu (KlaussCPUISelDAGToDAG.cpp): rule 4 makes W ops too.
extern cl::opt<bool> KlaussCPUEnableW32Alu;

namespace {

class KlaussCPUOptExtensions : public MachineFunctionPass {
public:
  static char ID;
  KlaussCPUOptExtensions() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return PASS_NAME; }

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::IsSSA);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // end anonymous namespace

char KlaussCPUOptExtensions::ID = 0;

INITIALIZE_PASS(KlaussCPUOptExtensions, DEBUG_TYPE, PASS_NAME, false, false)

// The width N of an extension instruction and whether it zero-extends, or 0.
static unsigned getExtension(unsigned Opc, bool &IsZExt) {
  IsZExt = true;
  switch (Opc) {
  case KlaussCPU::ZEXTB: return 8;
  case KlaussCPU::ZEXTH: return 16;
  case KlaussCPU::ZEXTW: return 32;
  default: break;
  }
  IsZExt = false;
  switch (Opc) {
  case KlaussCPU::SEXTB: return 8;
  case KlaussCPU::SEXTH: return 16;
  case KlaussCPU::SEXTW: return 32;
  default: return 0;
  }
}

static bool isBooleanCompare(unsigned Opc) {
  switch (Opc) {
  case KlaussCPU::CMPEQR:  case KlaussCPU::CMPEQV:
  case KlaussCPU::CMPNER:  case KlaussCPU::CMPNEV:
  case KlaussCPU::CMPLTR:  case KlaussCPU::CMPLTV:
  case KlaussCPU::CMPLER:  case KlaussCPU::CMPLEV:
  case KlaussCPU::CMPGTR:  case KlaussCPU::CMPGTV:
  case KlaussCPU::CMPGER:  case KlaussCPU::CMPGEV:
  case KlaussCPU::CMPULTR: case KlaussCPU::CMPULTV:
  case KlaussCPU::CMPULER: case KlaussCPU::CMPULEV:
  case KlaussCPU::CMPUGTR: case KlaussCPU::CMPUGTV:
  case KlaussCPU::CMPUGER: case KlaussCPU::CMPUGEV:
  case KlaussCPU::CMPEQRW:  case KlaussCPU::CMPEQVW:
  case KlaussCPU::CMPNERW:  case KlaussCPU::CMPNEVW:
  case KlaussCPU::CMPLTRW:  case KlaussCPU::CMPLTVW:
  case KlaussCPU::CMPLERW:  case KlaussCPU::CMPLEVW:
  case KlaussCPU::CMPGTRW:  case KlaussCPU::CMPGTVW:
  case KlaussCPU::CMPGERW:  case KlaussCPU::CMPGEVW:
  case KlaussCPU::CMPULTRW: case KlaussCPU::CMPULTVW:
  case KlaussCPU::CMPULERW: case KlaussCPU::CMPULEVW:
  case KlaussCPU::CMPUGTRW: case KlaussCPU::CMPUGTVW:
  case KlaussCPU::CMPUGERW: case KlaussCPU::CMPUGEVW:
    return true;
  default:
    return false;
  }
}

// Whether the immediate operand Op of MI is a constant (not a symbol), and
// its value as the instruction extends it to 64 bits.
static bool getImm(const MachineInstr &MI, unsigned Op, bool SExtImm,
                   int64_t &Val) {
  const MachineOperand &MO = MI.getOperand(Op);
  if (!MO.isImm())
    return false;
  Val = SExtImm ? SignExtend64<32>(MO.getImm())
                : static_cast<int64_t>(Lo_32(MO.getImm()));
  return true;
}

// Whether the 64-bit value V is the zero (ZExt) or sign extension of its low
// Bits bits.
static bool fits(int64_t V, unsigned Bits, bool ZExt) {
  return ZExt ? isUIntN(Bits, V) : isIntN(Bits, V);
}

// Whether Reg holds the zero (ZExt) or sign extension of its low Bits bits.
// The registers an instruction's result is built from go on a worklist that
// must all be extended; a register seen before is assumed to be (a PHI
// cycle), which holds when everything that enters the cycle is.
static bool isExtended(Register Reg, unsigned Bits, bool ZExt,
                       const MachineRegisterInfo &MRI) {
  SmallVector<Register, 8> Worklist{Reg};
  SmallSet<Register, 16> Visited;
  while (!Worklist.empty()) {
    Register R = Worklist.pop_back_val();
    if (!R.isVirtual())
      return false;
    if (!Visited.insert(R).second)
      continue;
    if (Visited.size() > 32)
      return false;
    const MachineInstr *MI = MRI.getVRegDef(R);
    if (!MI)
      return false;
    unsigned Opc = MI->getOpcode();
    // An extension from W bits is a zero extension from W or more, and a
    // sign extension from W (sign) or W + 1 (zero) or more.
    auto Extends = [&](unsigned W, bool WZExt) {
      return ZExt ? WZExt && W <= Bits : W + WZExt <= Bits;
    };
    bool IsZExt;
    if (unsigned W = getExtension(Opc, IsZExt)) {
      if (Extends(W, IsZExt))
        continue;
      return false;
    }
    if (isBooleanCompare(Opc)) {
      if (Extends(1, true))
        continue;
      return false;
    }
    int64_t Imm;
    switch (Opc) {
    case KlaussCPU::LDIDX8: case KlaussCPU::LDIDX8R: case KlaussCPU::MEMGET8:
    case KlaussCPU::LDABS8:
      if (Extends(8, true))
        continue;
      return false;
    case KlaussCPU::LDIDX16: case KlaussCPU::LDIDX16R:
    case KlaussCPU::MEMGET16: case KlaussCPU::LDABS16:
      if (Extends(16, true))
        continue;
      return false;
    case KlaussCPU::LDIDX32: case KlaussCPU::LDIDX32R:
    case KlaussCPU::MEMGET32: case KlaussCPU::LDABS32:
      if (Extends(32, true))
        continue;
      return false;
    case KlaussCPU::LDIDX8_S: case KlaussCPU::LDIDX8_SR:
    case KlaussCPU::LDABS8_S:
      if (Extends(8, false))
        continue;
      return false;
    case KlaussCPU::LDIDX16_S: case KlaussCPU::LDIDX16_SR:
    case KlaussCPU::LDABS16_S:
      if (Extends(16, false))
        continue;
      return false;
    case KlaussCPU::LDIDX32_S: case KlaussCPU::LDIDX32_SR:
    case KlaussCPU::LDABS32_S:
    case KlaussCPU::ADDW: case KlaussCPU::SUBW: case KlaussCPU::MULW:
    case KlaussCPU::ADDIW:
      if (Extends(32, false))
        continue;
      return false;
    case KlaussCPU::CLZ: case KlaussCPU::CTZ: case KlaussCPU::POPCNT:
      if (Extends(7, true))
        continue;
      return false;
    case KlaussCPU::BEXTR:
      // (x >> s) & (2^l - 1), l <= 31 (KlaussCPUISelDAGToDAG BitFieldExtract).
      if (Extends(31, true))
        continue;
      return false;
    case KlaussCPU::SETR:
    case KlaussCPU::SETRU:
      if (getImm(*MI, 1, Opc == KlaussCPU::SETR, Imm) && fits(Imm, Bits, ZExt))
        continue;
      return false;
    case KlaussCPU::SHRV:
    case KlaussCPU::SHRAV: {
      // The top Amt bits are zero (SHRV) or copies of the sign (SHRAV).
      unsigned Amt = MI->getOperand(2).getImm() & 63;
      if (Opc == KlaussCPU::SHRV ? Extends(64 - Amt, true)
                                 : Extends(64 - Amt, false))
        continue;
      // A shift right keeps an extension of the same kind from as many bits.
      if (ZExt == (Opc == KlaussCPU::SHRV)) {
        Worklist.push_back(MI->getOperand(1).getReg());
        continue;
      }
      return false;
    }
    case KlaussCPU::ANDV: case KlaussCPU::ANDI:
      // A mask that is itself extended from Bits bits makes the result so,
      // when it is non-negative.
      if (getImm(*MI, 2, Opc == KlaussCPU::ANDI, Imm) && Imm >= 0 &&
          (ZExt ? isUIntN(Bits, Imm) : isUIntN(Bits - 1, Imm)))
        continue;
      [[fallthrough]];
    case KlaussCPU::ORV: case KlaussCPU::ORI:
    case KlaussCPU::XORV: case KlaussCPU::XORI: {
      // A bitwise op keeps an extension both operands have.
      bool SExtImm = Opc == KlaussCPU::ANDI || Opc == KlaussCPU::ORI ||
                     Opc == KlaussCPU::XORI;
      if (getImm(*MI, 2, SExtImm, Imm) && fits(Imm, Bits, ZExt)) {
        Worklist.push_back(MI->getOperand(1).getReg());
        continue;
      }
      return false;
    }
    case KlaussCPU::ANDR: case KlaussCPU::ORR: case KlaussCPU::XORR:
    // A min or max is one of its operands.
    case KlaussCPU::MINR: case KlaussCPU::MAXR:
    case KlaussCPU::MINUR: case KlaussCPU::MAXUR:
      Worklist.push_back(MI->getOperand(1).getReg());
      Worklist.push_back(MI->getOperand(2).getReg());
      continue;
    case KlaussCPU::NOTR:
      // ~x of a sign extension is one; of a zero extension it is not.
      if (ZExt)
        return false;
      Worklist.push_back(MI->getOperand(1).getReg());
      continue;
    case KlaussCPU::COPY:
    case KlaussCPU::COPY_R:
      Worklist.push_back(MI->getOperand(1).getReg());
      continue;
    case KlaussCPU::PHI:
      for (unsigned I = 1, E = MI->getNumOperands(); I < E; I += 2)
        Worklist.push_back(MI->getOperand(I).getReg());
      continue;
    default:
      return false;
    }
  }
  return true;
}

// Whether MI's FLAGS result, if it has one, is unused.
static bool flagsDead(const MachineInstr &MI) {
  int Idx = MI.findRegisterDefOperandIdx(KlaussCPU::FLAGS, /*TRI=*/nullptr);
  return Idx < 0 || MI.getOperand(Idx).isDead();
}

// The number of low bits of operand OpNo of MI the instruction reads, 64 if
// it reads them all; and in Through the register whose own readers decide,
// when the low bits of MI's result depend only on the low bits of its
// operands.
static unsigned getReadWidth(const MachineInstr &MI, unsigned OpNo,
                             Register &Through) {
  Through = Register();
  unsigned Opc = MI.getOpcode();
  bool IsZExt;
  if (unsigned W = getExtension(Opc, IsZExt))
    return W;
  switch (Opc) {
  // The data operand of a store.
  case KlaussCPU::STIDX8: case KlaussCPU::STIDX8R: case KlaussCPU::MEMSET8:
    return OpNo == 0 ? 8 : 64;
  case KlaussCPU::STIDX16: case KlaussCPU::STIDX16R: case KlaussCPU::MEMSET16:
    return OpNo == 0 ? 16 : 64;
  case KlaussCPU::STIDX32: case KlaussCPU::STIDX32R: case KlaussCPU::MEMSET32:
    return OpNo == 0 ? 32 : 64;
  // The W ops and compares read the low 32 bits of every operand.
  case KlaussCPU::ADDW: case KlaussCPU::SUBW: case KlaussCPU::MULW:
  case KlaussCPU::ADDIW:
  case KlaussCPU::CMPRRW_I: case KlaussCPU::CMPRVW_I:
  case KlaussCPU::CMPEQRW:  case KlaussCPU::CMPEQVW:
  case KlaussCPU::CMPNERW:  case KlaussCPU::CMPNEVW:
  case KlaussCPU::CMPLTRW:  case KlaussCPU::CMPLTVW:
  case KlaussCPU::CMPLERW:  case KlaussCPU::CMPLEVW:
  case KlaussCPU::CMPGTRW:  case KlaussCPU::CMPGTVW:
  case KlaussCPU::CMPGERW:  case KlaussCPU::CMPGEVW:
  case KlaussCPU::CMPULTRW: case KlaussCPU::CMPULTVW:
  case KlaussCPU::CMPULERW: case KlaussCPU::CMPULEVW:
  case KlaussCPU::CMPUGTRW: case KlaussCPU::CMPUGTVW:
  case KlaussCPU::CMPUGERW: case KlaussCPU::CMPUGEVW:
    return 32;
  // A register shift or rotate amount: rs2[5:0].
  case KlaussCPU::SHLR: case KlaussCPU::SHRR: case KlaussCPU::SARR:
  case KlaussCPU::ROLR: case KlaussCPU::RORR_R:
    return OpNo == 2 ? 6 : 64;
  // The low bits of the result come from the low bits of the operands.
  case KlaussCPU::ADDR: case KlaussCPU::SUBR: case KlaussCPU::MULR:
  case KlaussCPU::ANDR: case KlaussCPU::ORR: case KlaussCPU::XORR:
  case KlaussCPU::ADDI: case KlaussCPU::ADDV: case KlaussCPU::MINUSV:
  case KlaussCPU::ANDI: case KlaussCPU::ANDV: case KlaussCPU::ORI:
  case KlaussCPU::ORV: case KlaussCPU::XORI: case KlaussCPU::XORV:
  case KlaussCPU::MULV: case KlaussCPU::SHLV:
  case KlaussCPU::INCR: case KlaussCPU::DECR: case KlaussCPU::NEGR:
  case KlaussCPU::NOTR:
    if (!flagsDead(MI))
      return 64;
    Through = MI.getOperand(0).getReg();
    return 0;
  case KlaussCPU::COPY:
  case KlaussCPU::COPY_R:
  case KlaussCPU::PHI:
    Through = MI.getOperand(0).getReg();
    return 0;
  default:
    return 64;
  }
}

// Whether everything that reads Reg reads only its low Bits bits.  Reached,
// if given, collects Reg and the registers followed from it, whose values
// change above bit Bits once Reg's do.
static bool onlyLowBitsRead(Register Reg, unsigned Bits,
                            const MachineRegisterInfo &MRI,
                            SmallVectorImpl<Register> *Reached = nullptr) {
  SmallVector<Register, 8> Worklist{Reg};
  SmallSet<Register, 16> Visited;
  while (!Worklist.empty()) {
    Register R = Worklist.pop_back_val();
    if (!R.isVirtual())
      return false;
    if (!Visited.insert(R).second)
      continue;
    if (Visited.size() > 32)
      return false;
    if (Reached)
      Reached->push_back(R);
    for (const MachineOperand &MO : MRI.use_nodbg_operands(R)) {
      const MachineInstr &UseMI = *MO.getParent();
      Register Through;
      unsigned W = getReadWidth(UseMI, UseMI.getOperandNo(&MO), Through);
      if (Through) {
        Worklist.push_back(Through);
        continue;
      }
      if (W > Bits)
        return false;
    }
  }
  return true;
}

// Ext extends a PHI.  When every other reader of the PHI is the same
// extension or reads only the low bits, the PHI may as well carry the
// extended values: then Ext is redundant (rule 1).  An incoming value that is
// not extended already must have the same extension in its predecessor
// block, which becomes the incoming value instead; nothing is added.  This is
// the loop `idx = (u32)b[idx]`, whose loaded index is zero-extended in the
// latch for another use and again, through the PHI, at the top of the loop.
// Changed is set to the PHI and the registers followed from it, whose values
// change above bit Bits.
static bool extendPHIInputs(MachineInstr &Ext, unsigned Bits, bool ZExt,
                            MachineRegisterInfo &MRI,
                            SmallVectorImpl<Register> &Changed) {
  Register Src = Ext.getOperand(1).getReg();
  MachineInstr *Phi = MRI.getVRegDef(Src);
  if (!Phi || !Phi->isPHI())
    return false;
  unsigned Opc = Ext.getOpcode();
  Changed.assign(1, Src);
  for (const MachineOperand &MO : MRI.use_nodbg_operands(Src)) {
    const MachineInstr &UseMI = *MO.getParent();
    if (UseMI.getOpcode() == Opc)
      continue;
    Register Through;
    unsigned W = getReadWidth(UseMI, UseMI.getOperandNo(&MO), Through);
    if (Through ? !onlyLowBitsRead(Through, Bits, MRI, &Changed) : W > Bits)
      return false;
  }
  SmallVector<std::pair<unsigned, Register>, 4> NewInputs;
  for (unsigned I = 1, E = Phi->getNumOperands(); I < E; I += 2) {
    Register In = Phi->getOperand(I).getReg();
    if (isExtended(In, Bits, ZExt, MRI))
      continue;
    MachineBasicBlock *Pred = Phi->getOperand(I + 1).getMBB();
    Register Extended;
    for (const MachineInstr &UseMI : MRI.use_nodbg_instructions(In))
      if (UseMI.getOpcode() == Opc && UseMI.getParent() == Pred &&
          UseMI.getOperand(1).getReg() == In && flagsDead(UseMI)) {
        Extended = UseMI.getOperand(0).getReg();
        break;
      }
    if (!Extended || MRI.getRegClass(Extended) != MRI.getRegClass(Src))
      return false;
    NewInputs.push_back({I, Extended});
  }
  if (NewInputs.empty())
    return false;
  for (auto [I, Extended] : NewInputs) {
    LLVM_DEBUG(dbgs() << "PHI input " << printReg(Extended) << " for "
                      << printReg(Phi->getOperand(I).getReg()) << " in "
                      << *Phi);
    Phi->getOperand(I).setReg(Extended);
    // Its last reader in the predecessor no longer kills it.
    MRI.clearKillFlags(Extended);
  }
  MRI.clearKillFlags(Src);
  return true;
}

// Rule 2: Dst, the extension of Src, is replaced by Src, whose bits from Bits
// up differ.  Its debug values describe Src extended instead.
static void extendDebugValues(Register Dst, Register Src, unsigned Bits,
                              bool ZExt, MachineRegisterInfo &MRI) {
  for (MachineOperand &MO : make_early_inc_range(MRI.use_operands(Dst))) {
    MachineInstr &DbgMI = *MO.getParent();
    if (!DbgMI.isDebugValue())
      continue;
    const DIExpression *Expr = DbgMI.getDebugExpression();
    if (DbgMI.isIndirectDebugValue() || Expr->isEntryValue()) {
      DbgMI.setDebugValueUndef();
      continue;
    }
    DIExpression::ExtOps Ops = DIExpression::getExtOps(Bits, 64, !ZExt);
    DbgMI.getDebugExpressionOp().setMetadata(DIExpression::appendOpsToArg(
        Expr, Ops, DbgMI.getDebugOperandIndex(&MO), /*StackValue=*/true));
    MO.setReg(Src);
  }
}

// Whether DbgMI shows its register's low Bits bits and no more: a plain
// DBG_VALUE of a register location, of a variable (or piece) that size or
// smaller.
static bool showsLowBitsOnly(const MachineInstr &DbgMI, unsigned Bits) {
  if (!DbgMI.isNonListDebugValue() || DbgMI.isIndirectDebugValue())
    return false;
  const DIExpression *Expr = DbgMI.getDebugExpression();
  for (auto Op : Expr->expr_ops())
    if (Op.getOp() != dwarf::DW_OP_LLVM_fragment)
      return false;
  std::optional<uint64_t> Size = DbgMI.getDebugVariable()->getSizeInBits();
  if (auto Frag = Expr->getFragmentInfo())
    Size = Frag->SizeInBits;
  return Size && *Size <= Bits;
}

// The debug values of Regs, whose bits from Bits up change, become undef
// where they show those bits.
static void dropWideDebugValues(ArrayRef<Register> Regs, unsigned Bits,
                                MachineRegisterInfo &MRI) {
  for (Register R : Regs)
    for (MachineInstr &DbgMI : MRI.use_instructions(R))
      if (DbgMI.isDebugValue() && !showsLowBitsOnly(DbgMI, Bits))
        DbgMI.setDebugValueUndef();
}

// Rule 4: Src, which a SEXTW extends, is an add, subtract or multiply whose
// readers all read only its low 32 bits.  Replace it with its W form, whose
// low 32 bits are the same and whose result is their sign extension.
// Not for an ADDI of a frame index.  Changed is set to Src and the registers
// followed from it, whose values change above bit 32.
static bool makeWOp(Register Src, MachineRegisterInfo &MRI,
                    const TargetInstrInfo &TII,
                    SmallVectorImpl<Register> &Changed) {
  MachineInstr *MI = MRI.getVRegDef(Src);
  if (!MI || !flagsDead(*MI))
    return false;
  // An ADDI of a frame index stays: eliminateFrameIndex folds the slot offset
  // into an ADDI but would compute an ADDIW's base in a scratch register
  // first, which costs more than the SEXTW.
  if (!MI->getOperand(1).isReg())
    return false;
  unsigned WOpc;
  bool HasImm = false;
  int64_t Imm = 0;
  switch (MI->getOpcode()) {
  case KlaussCPU::ADDR: WOpc = KlaussCPU::ADDW; break;
  case KlaussCPU::SUBR: WOpc = KlaussCPU::SUBW; break;
  case KlaussCPU::MULR: WOpc = KlaussCPU::MULW; break;
  // The low 32 bits of x + c depend only on the low 32 bits of c, so every
  // immediate form becomes ADDIW of that 32-bit pattern.
  case KlaussCPU::ADDI:
  case KlaussCPU::ADDV:
  case KlaussCPU::MINUSV:
    if (!MI->getOperand(2).isImm())
      return false;
    WOpc = KlaussCPU::ADDIW;
    HasImm = true;
    Imm = MI->getOperand(2).getImm();
    if (MI->getOpcode() == KlaussCPU::MINUSV)
      Imm = -Imm;
    break;
  case KlaussCPU::INCR:
  case KlaussCPU::DECR:
    WOpc = KlaussCPU::ADDIW;
    HasImm = true;
    Imm = MI->getOpcode() == KlaussCPU::INCR ? 1 : -1;
    break;
  default:
    return false;
  }
  Changed.clear();
  if (!onlyLowBitsRead(Src, 32, MRI, &Changed))
    return false;
  MachineInstrBuilder MIB =
      BuildMI(*MI->getParent(), MI, MI->getDebugLoc(), TII.get(WOpc), Src)
          .add(MI->getOperand(1));
  if (HasImm)
    MIB.addImm(static_cast<int32_t>(Imm));
  else
    MIB.add(MI->getOperand(2));
  if (MachineOperand *F =
          MIB->findRegisterDefOperand(KlaussCPU::FLAGS, /*TRI=*/nullptr))
    F->setIsDead();
  LLVM_DEBUG(dbgs() << "W op " << *MIB << "  for " << *MI);
  MI->eraseFromParent();
  return true;
}

bool KlaussCPUOptExtensions::runOnMachineFunction(MachineFunction &MF) {
  if (!EnableOptExtensions || skipFunction(MF.getFunction()))
    return false;

  MachineRegisterInfo &MRI = MF.getRegInfo();
  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : make_early_inc_range(MBB)) {
      bool IsZExt;
      unsigned Bits = getExtension(MI.getOpcode(), IsZExt);
      if (!Bits || !flagsDead(MI))
        continue;
      Register Dst = MI.getOperand(0).getReg();
      Register Src = MI.getOperand(1).getReg();
      if (!Dst.isVirtual() || !Src.isVirtual() ||
          MRI.getRegClass(Dst) != MRI.getRegClass(Src))
        continue;
      // Registers whose bits from Bits up change (rules 2 and 3).
      SmallVector<Register, 8> Altered;
      if (isExtended(Src, Bits, IsZExt, MRI)) {
        ++NumExtendedSource;
      } else if (onlyLowBitsRead(Dst, Bits, MRI, &Altered)) {
        ++NumNarrowUsers;
        extendDebugValues(Dst, Src, Bits, IsZExt, MRI);
      } else if (extendPHIInputs(MI, Bits, IsZExt, MRI, Altered)) {
        ++NumPHIInputs;
      } else if (KlaussCPUEnableW32Alu &&
                 MI.getOpcode() == KlaussCPU::SEXTW &&
                 makeWOp(Src, MRI, TII, Altered)) {
        ++NumWOps;
      } else {
        continue;
      }
      dropWideDebugValues(Altered, Bits, MRI);
      LLVM_DEBUG(dbgs() << "Removing " << MI);
      MRI.replaceRegWith(Dst, Src);
      MRI.clearKillFlags(Src);
      MI.eraseFromParent();
      Changed = true;
    }
  }
  return Changed;
}

FunctionPass *llvm::createKlaussCPUOptExtensionsPass() {
  return new KlaussCPUOptExtensions();
}
