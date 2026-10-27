//===-- KlaussCPUISelDAGToDAG.cpp - KlaussCPU DAG->DAG Instruction Selector ===//
//
// KlaussCPU LLVM backend — DAG-to-DAG instruction selection pass.
//
// Instruction selection is the tablegen-generated SelectCode() (from
// KlaussCPUGenDAGISel.inc), driven by the isel patterns in
// KlaussCPUInstrInfo.td.  This file holds the pass scaffolding, the
// ComplexPattern hook (SelectAddrFI), the inline-asm memory operand hook and
// the two nodes Select() handles itself:
//   BR_CC, KlaussCPUISD::SELECT_CC: they pick the 32-bit W compare by looking
//     at how each operand was extended and whether stripping the extension
//     removes an instruction.  BR_CC also glues the compare (which defines
//     only FLAGS) to its conditional jump; as a pattern that would need a
//     FLAGS-producing compare node, and the W-compare choice would still be
//     C++, so both nodes stay here.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUTargetMachine.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/CodeGen/SelectionDAGNodes.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"

// KlaussCPU:: opcode enum (CMPRR_I, LDIDX64, STIDX64, etc.) is visible
// transitively: KlaussCPUTargetMachine.h → KlaussCPUSubtarget.h →
// KlaussCPUInstrInfo.h → GET_INSTRINFO_ENUM → KlaussCPUGenInstrInfo.inc.
// Do NOT re-include KlaussCPUGenInstrInfo.inc here — it would redefine the enum.

#define DEBUG_TYPE "klausscpu-isel"
#define PASS_NAME "KlaussCPU DAG->DAG Pattern Instruction Selection"

namespace llvm {

// Forward declaration required before INITIALIZE_PASS expands the definition.
void initializeKlaussCPUDAGToDAGISelLegacyPass(PassRegistry &);
FunctionPass *createKlaussCPUISelDag(KlaussCPUTargetMachine &TM);

} // namespace llvm

using namespace llvm;

#include "llvm/Support/CommandLine.h"
static cl::opt<bool> EnableW32Alu(
    "klausscpu-w32-alu", cl::init(true), cl::Hidden,
    cl::desc("ISA v3 D3: select ADDW/SUBW/ADDIW/MULW for sign-extended 32-bit "
             "add/sub/mul"));
static cl::opt<bool> FoldFrameIndexOffset(
    "klausscpu-fold-fi-offset", cl::init(true), cl::Hidden,
    cl::desc("Fold a frame index plus a constant into the offset of a "
             "load, store or frame-address ADDI"));
static cl::opt<bool> EnableW32Compare(
    "klausscpu-w32-compare", cl::init(true), cl::Hidden,
    cl::desc("ISA v3 D2: select 32-bit CMPRRW/CMPRVW for compares of "
             "32-bit-extended values"));

namespace {
// ISA v3 D2: how a compare operand was extended from 32 bits.
//   S/Z = its value is the sign/zero extension of its low 32 bits.
//   Stripped = the extension is an explicit node we can drop (Inner is the
//   unextended value); otherwise Inner is the operand itself.
struct Ext32 {
  enum Kind { None, S, Z } K = None;
  SDValue Inner;
  SDNode *Node = nullptr;
  bool Stripped = false;
};
} // namespace

static Ext32 classifyExt32(SDValue V);

/// ISA v3 D2: can this compare use the 32-bit W form?  If so, strip the
/// explicit extensions from LHS/RHS in place (RHS may stay a constant) and
/// return true.  Sign-extension preserves both signed and unsigned 32-bit
/// order, so a sext pair works for any condition; a zext pair only for
/// EQ/NE and the unsigned conditions.  Only used when an extension
/// instruction actually disappears.
static bool tryW32Compare(ISD::CondCode CC, SDValue &LHS, SDValue &RHS) {
  if (!EnableW32Compare)
    return false;
  Ext32 L = classifyExt32(LHS), R = classifyExt32(RHS);
  auto *RC = dyn_cast<ConstantSDNode>(RHS);
  bool Unsig = CC == ISD::SETEQ || CC == ISD::SETNE || CC == ISD::SETULT ||
               CC == ISD::SETULE || CC == ISD::SETUGT || CC == ISD::SETUGE;
  Ext32::Kind K = Ext32::None;
  if (RC) {
    int64_t V = RC->getSExtValue();
    if (L.K == Ext32::S && isInt<32>(V))
      K = Ext32::S;
    else if (L.K == Ext32::Z && isUInt<32>(static_cast<uint64_t>(V)) && Unsig)
      K = Ext32::Z;
  } else if (L.K == R.K && L.K != Ext32::None && (L.K == Ext32::S || Unsig)) {
    K = L.K;
  }
  bool Saves = (L.Stripped && L.Node->hasOneUse()) ||
               (!RC && R.Stripped && R.Node->hasOneUse());
  if (K == Ext32::None || !Saves)
    return false;
  LHS = L.Inner;
  if (!RC)
    RHS = R.Inner;
  return true;
}

static Ext32 classifyExt32(SDValue V) {
  Ext32 E;
  E.Inner = V;
  E.Node = V.getNode();
  switch (V.getOpcode()) {
  case ISD::SIGN_EXTEND_INREG: {
    EVT VT = cast<VTSDNode>(V.getOperand(1))->getVT();
    if (VT == MVT::i32) {
      E.K = Ext32::S; E.Inner = V.getOperand(0); E.Stripped = true;
    } else if (VT.getSizeInBits() < 32) {
      E.K = Ext32::S;
    }
    break;
  }
  case ISD::AND:
    if (auto *C = dyn_cast<ConstantSDNode>(V.getOperand(1))) {
      uint64_t M = C->getZExtValue();
      if (M == 0xFFFFFFFFull) {
        E.K = Ext32::Z; E.Inner = V.getOperand(0); E.Stripped = true;
      } else if (M <= 0xFFFFFFFFull) {
        E.K = Ext32::Z; // narrower mask: already zero-extended from 32
      }
    }
    break;
  case ISD::AssertSext:
    if (cast<VTSDNode>(V.getOperand(1))->getVT().getSizeInBits() <= 32)
      E.K = Ext32::S;
    break;
  case ISD::AssertZext:
    if (cast<VTSDNode>(V.getOperand(1))->getVT().getSizeInBits() <= 32)
      E.K = Ext32::Z;
    break;
  case ISD::LOAD: {
    auto *LN = cast<LoadSDNode>(V.getNode());
    if (LN->getMemoryVT().getSizeInBits() <= 32) {
      if (LN->getExtensionType() == ISD::SEXTLOAD)
        E.K = Ext32::S;
      else if (LN->getExtensionType() == ISD::ZEXTLOAD)
        E.K = Ext32::Z;
    }
    break;
  }
  default:
    break;
  }
  return E;
}

//===----------------------------------------------------------------------===//
// KlaussCPU-specific DAG-to-DAG instruction selector.
//===----------------------------------------------------------------------===//
namespace {

class KlaussCPUDAGToDAGISel : public SelectionDAGISel {
public:
  explicit KlaussCPUDAGToDAGISel(KlaussCPUTargetMachine &TM)
      : SelectionDAGISel(TM) {}

  void Select(SDNode *N) override;

  bool SelectAddrFI(SDValue Addr, SDValue &Base, SDValue &Offset) {
    return selectFrameAddr(Addr, /*Scale=*/1, Base, Offset);
  }
  bool SelectAddrFIMem(SDNode *Parent, SDValue Addr, SDValue &Base,
                       SDValue &Offset) {
    unsigned Scale = cast<MemSDNode>(Parent)->getMemoryVT().getStoreSize();
    return selectFrameAddr(Addr, Scale, Base, Offset);
  }
  bool selectFrameAddr(SDValue Addr, unsigned Scale, SDValue &Base,
                       SDValue &Offset);
  bool frameOffsetFitsShortForm(int FI, int64_t C, unsigned Scale) const;

  bool SelectInlineAsmMemoryOperand(const SDValue &Op,
                                    InlineAsm::ConstraintCode ConstraintID,
                                    std::vector<SDValue> &OutOps) override;

  // Auto-generated tablegen instruction selection tables.
#include "KlaussCPUGenDAGISel.inc"
};

class KlaussCPUDAGToDAGISelLegacy : public SelectionDAGISelLegacy {
public:
  static char ID;
  explicit KlaussCPUDAGToDAGISelLegacy(KlaussCPUTargetMachine &TM)
      : SelectionDAGISelLegacy(ID,
                               std::make_unique<KlaussCPUDAGToDAGISel>(TM)) {}
};

} // end anonymous namespace

char KlaussCPUDAGToDAGISelLegacy::ID = 0;

INITIALIZE_PASS(KlaussCPUDAGToDAGISelLegacy, DEBUG_TYPE, PASS_NAME,
                /*CFGOnly=*/false, /*isAnalysis=*/false)

void KlaussCPUDAGToDAGISel::Select(SDNode *N) {
  // Already selected — leave it alone.
  if (N->isMachineOpcode()) {
    LLVM_DEBUG(dbgs() << "== "; N->dump(CurDAG); dbgs() << "\n");
    return;
  }

  // ---- ISD::BR_CC → CMPRR_I/CMPRV_I + conditional JMP -------------------
  // ISD::BR_CC: (chain, condcode, lhs, rhs, dest_bb)
  //
  // The compare instruction has no output register (it only sets processor
  // flags): it is chained and glued to the branch (see below).
  //
  // CMPRV_I accepts a signed-32-bit immediate RHS directly, CMPRVU_I one in
  // [2^31, 2^32) (zero-extended).  CMPRR_I requires both operands to be in
  // registers; any other constant is left in RHS, which the SETR64 pattern
  // materialises.
  if (N->getOpcode() == ISD::BR_CC) {
    ISD::CondCode CC = cast<CondCodeSDNode>(N->getOperand(1))->get();
    SDValue Chain    = N->getOperand(0);
    SDValue LHS      = N->getOperand(2);
    SDValue RHS      = N->getOperand(3);
    SDValue Dest     = N->getOperand(4);
    SDLoc DL(N);

    // A3a (arithmetic Z-flag reuse) is NOT done here.  A DAG-level attempt
    // to drop the compare and branch on the arithmetic op's Z flag has to
    // glue the arithmetic op adjacent to the branch, which is unschedulable
    // whenever that op's result also feeds a loop back-edge (the CopyToReg has
    // nowhere to go) — it deadlocks the DAG scheduler.  A3a is instead a
    // post-RA MI peephole (KlaussCPUFlagReuse.cpp) that rewrites the already-
    // scheduled `<arith Rd>; CMPRV Rd,0; JMP{E,NE}` triple into
    // `<arith Rd>; JMP{Z,NZ}`, where adjacency in the final stream proves no
    // flag-clobbering instruction sits between the producer and the branch.

    // Emit the compare, glued to the branch.  The glue is what tells
    // InstrEmitter that the branch reads the compare's implicit $flags def;
    // with only a chain edge the def is emitted `dead` and the branch then
    // reads an undefined physreg (fails -verify-machineinstrs, and lets
    // post-RA passes treat the flags as free between compare and branch).
    SDVTList CmpVTs = CurDAG->getVTList(MVT::Other, MVT::Glue);
    SDNode *CmpNode;

    // ISA v3 D2: 32-bit compare when both operands are 32-bit extended.
    if (tryW32Compare(CC, LHS, RHS)) {
      if (auto *C = dyn_cast<ConstantSDNode>(RHS)) {
        SDValue Imm = CurDAG->getTargetConstant(
            static_cast<int32_t>(C->getZExtValue()), DL, MVT::i64);
        SDValue Ops[] = {LHS, Imm, Chain};
        CmpNode = CurDAG->getMachineNode(KlaussCPU::CMPRVW_I, DL, CmpVTs, Ops);
      } else {
        SDValue Ops[] = {LHS, RHS, Chain};
        CmpNode = CurDAG->getMachineNode(KlaussCPU::CMPRRW_I, DL, CmpVTs, Ops);
      }
    } else
    if (auto *C = dyn_cast<ConstantSDNode>(RHS);
        C && isInt<32>(C->getSExtValue())) {
      // Constant RHS that fits in a signed 32-bit field → CMPRV_I.
      SDValue Imm  = CurDAG->getTargetConstant(C->getSExtValue(), DL, MVT::i64);
      SDValue Ops[] = {LHS, Imm, Chain};
      CmpNode = CurDAG->getMachineNode(KlaussCPU::CMPRV_I, DL, CmpVTs, Ops);
    } else if (auto *C = dyn_cast<ConstantSDNode>(RHS);
               C && isUInt<32>(C->getZExtValue())) {
      SDValue Imm  = CurDAG->getTargetConstant(C->getZExtValue(), DL, MVT::i64);
      SDValue Ops[] = {LHS, Imm, Chain};
      CmpNode = CurDAG->getMachineNode(KlaussCPU::CMPRVU_I, DL, CmpVTs, Ops);
    } else {
      // Register-register compare.
      SDValue Ops[] = {LHS, RHS, Chain};
      CmpNode = CurDAG->getMachineNode(KlaussCPU::CMPRR_I, DL, CmpVTs, Ops);
    }

    // Map ISD::CondCode to the KlaussCPU conditional jump opcode (see
    // KlaussCPUInstrInfo::getCondBranchOpcode for the flag derivations).
    unsigned JmpOpc = KlaussCPUInstrInfo::getCondBranchOpcode(
        CC, TM.isPositionIndependent());
    if (!JmpOpc)
      llvm_unreachable("KlaussCPU BR_CC: unhandled CondCode");

    SDValue JmpOps[] = {Dest, SDValue(CmpNode, 0) /* chain */,
                        SDValue(CmpNode, 1) /* glue: reads $flags */};
    SDNode *JmpNode = CurDAG->getMachineNode(JmpOpc, DL, MVT::Other, JmpOps);
    ReplaceNode(N, JmpNode);
    return;
  }

  // ---- KlaussCPUISD::SELECT_CC → SELECT_CC_RR/RI (custom inserter) --------
  // (lhs, rhs, cc, trueV, falseV).  The pseudo carries the conditional-jump
  // opcode directly; the inserter emits CMPRR/CMPRV/CMPRVU + that jump.
  if (N->getOpcode() == KlaussCPUISD::SELECT_CC) {
    SDLoc DL(N);
    SDValue LHS = N->getOperand(0);
    SDValue RHS = N->getOperand(1);
    ISD::CondCode CC = cast<CondCodeSDNode>(N->getOperand(2))->get();
    unsigned JmpOpc = KlaussCPUInstrInfo::getCondBranchOpcode(
        CC, TM.isPositionIndependent());
    assert(JmpOpc && "SELECT_CC with unsupported CondCode");
    // ISA v3 D2: bit 31 of the $jmp immediate asks the inserter for the
    // 32-bit CMPRRW/CMPRVW compare (operands already stripped).
    bool W = tryW32Compare(CC, LHS, RHS);
    SDValue Jmp = CurDAG->getTargetConstant(
        JmpOpc | (W ? KlaussCPU::SelectCCW32Flag : 0), DL, MVT::i64);
    // SELECT_CC_RI takes a W compare's 32-bit pattern (printed signed), or a
    // 64-bit RHS in simm32 (CMPRV) or [2^31, 2^32) (CMPRVU).
    unsigned Opc = KlaussCPU::SELECT_CC_RR;
    if (auto *C = dyn_cast<ConstantSDNode>(RHS);
        C && (isUInt<32>(C->getZExtValue()) || isInt<32>(C->getSExtValue()))) {
      Opc = KlaussCPU::SELECT_CC_RI;
      RHS = CurDAG->getTargetConstant(
          W ? static_cast<int64_t>(static_cast<int32_t>(C->getZExtValue()))
            : C->getSExtValue(), DL, MVT::i64);
    }
    SDNode *Res = CurDAG->getMachineNode(
        Opc, DL, N->getValueType(0),
        {LHS, RHS, Jmp, N->getOperand(3), N->getOperand(4)});
    ReplaceNode(N, Res);
    return;
  }

  // Delegate everything else to the tablegen-generated SelectCode().
  SelectCode(N);
}

// A constant is folded onto a frame index only while slot offset + constant
// is sure to fit in simm32: |constant| < 512 MiB, four times the address
// space, so only a frame over 1.5 GiB could overflow (eliminateFrameIndex
// copes with that too, except at an inline asm).
static bool isFrameIndexOffset(int64_t C) { return isInt<30>(C); }

// Would frame object FI + C be in reach of the 1-word short form (a signed
// 8-bit offset scaled by the access size, unscaled for ADDI) once the frame
// is laid out?  An incoming-argument slot's offset is already known.  A
// local's is not, but PEI places the callee-saved slots (at most R4-R7)
// nearest the frame register, then the locals in index order and the spill
// slots, which are created later, below them: the local is no further away
// than the callee-saved area plus the size of every local up to and
// including it.  This is only a size estimate; any offset is correct.
bool KlaussCPUDAGToDAGISel::frameOffsetFitsShortForm(int FI, int64_t C,
                                                     unsigned Scale) const {
  const MachineFrameInfo &MFI = MF->getFrameInfo();
  int64_t Lo = -128 * int64_t(Scale), Hi = 127 * int64_t(Scale);
  if (MFI.isFixedObjectIndex(FI)) {
    // getFrameIndexReference: fixed objects are R15 + 8 + their offset.
    int64_t Off = MFI.getObjectOffset(FI) + 8 + C;
    return Off >= Lo && Off <= Hi;
  }
  int64_t Dist = 4 * 8;
  for (int I = 0; I <= FI; ++I)
    if (!MFI.isDeadObjectIndex(I) && !MFI.isVariableSizedObjectIndex(I))
      Dist += MFI.getObjectSize(I) + MFI.getObjectAlign(I).value() - 1;
  // The slot starts somewhere in [R15 - Dist, R15 - 1].
  return C - Dist >= Lo && C - 1 <= Hi;
}

// AddrFI / AddrFIMem: a frame index, or a frame index plus a constant (add,
// or a disjoint or), as (TargetFrameIndex, offset).  eliminateFrameIndex
// rewrites the pair to (frame register, slot offset + offset).  As a value it
// is one ADDI rd, FI, offset (AddrFI); a load or store of Scale bytes takes
// it as its base and offset (AddrFIMem).
//
// Folding the constant saves the ADDI that computes the slot address, but an
// access whose R15-relative offset is out of short-form reach takes 2 words,
// where through the slot address in a register its small offset takes 1.
// So the constant is folded when the offset is likely to be in reach, or
// when the frame index has at most two accesses in this block (two 2-word
// accesses are no larger than ADDI + two 1-word ones).  Otherwise the
// GPR-address patterns compute the slot address once.
bool KlaussCPUDAGToDAGISel::selectFrameAddr(SDValue Addr, unsigned Scale,
                                            SDValue &Base, SDValue &Offset) {
  int64_t C = 0;
  if (CurDAG->isBaseWithConstantOffset(Addr)) {
    auto *FIN = dyn_cast<FrameIndexSDNode>(Addr.getOperand(0));
    C = cast<ConstantSDNode>(Addr.getOperand(1))->getSExtValue();
    if (!FIN || !FoldFrameIndexOffset || !isFrameIndexOffset(C))
      return false;
    if (!frameOffsetFitsShortForm(FIN->getIndex(), C, Scale)) {
      // Count the users of FI and of each FI + constant.
      unsigned Accesses = 0;
      for (SDNode *U : FIN->users()) {
        SDValue UV(U, 0);
        bool FIPlusConst = U->getNumOperands() == 2 &&
                           U->getOperand(0).getNode() == FIN &&
                           CurDAG->isBaseWithConstantOffset(UV);
        Accesses += FIPlusConst ? U->use_size() : 1;
      }
      if (Accesses > 2)
        return false;
    }
    Addr = Addr.getOperand(0);
  }
  auto *FIN = dyn_cast<FrameIndexSDNode>(Addr);
  if (!FIN)
    return false;
  Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), MVT::i64);
  Offset = CurDAG->getTargetConstant(C, SDLoc(Addr), MVT::i64);
  return true;
}

// Inline asm memory operands ('m', 'o') become a (base, simm32 offset) pair,
// printed by KlaussCPUAsmPrinter::PrintAsmMemoryOperand as `base, offset` --
// the operand syntax of the LDIDX*/STIDX* instructions.  A frame index base
// stays a TargetFrameIndex followed by its offset, which eliminateFrameIndex
// rewrites to (FrameReg, slot offset + offset).  The constant is limited as
// for AddrFI (isFrameIndexOffset): eliminateFrameIndex cannot use a scratch
// register at the asm, where every GPR may be clobbered or used.  A larger
// constant stays in the address, which the register allocator computes like
// any other value.
bool KlaussCPUDAGToDAGISel::SelectInlineAsmMemoryOperand(
    const SDValue &Op, InlineAsm::ConstraintCode ConstraintID,
    std::vector<SDValue> &OutOps) {
  switch (ConstraintID) {
  case InlineAsm::ConstraintCode::o:
  case InlineAsm::ConstraintCode::m:
    break;
  default:
    return true;
  }

  SDLoc DL(Op);
  SDValue Base = Op;
  int64_t Offset = 0;
  if (CurDAG->isBaseWithConstantOffset(Op)) {
    int64_t C = cast<ConstantSDNode>(Op.getOperand(1))->getSExtValue();
    bool FIBase = isa<FrameIndexSDNode>(Op.getOperand(0));
    if (FIBase ? isFrameIndexOffset(C) : isInt<32>(C)) {
      Base = Op.getOperand(0);
      Offset = C;
    }
  }
  if (auto *FIN = dyn_cast<FrameIndexSDNode>(Base))
    Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), MVT::i64);

  OutOps.push_back(Base);
  OutOps.push_back(CurDAG->getTargetConstant(Offset, DL, MVT::i64));
  return false;
}

// Factory function called from KlaussCPUPassConfig::addInstSelector().
FunctionPass *llvm::createKlaussCPUISelDag(KlaussCPUTargetMachine &TM) {
  return new KlaussCPUDAGToDAGISelLegacy(TM);
}
