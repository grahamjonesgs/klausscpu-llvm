//===-- KlaussCPUISelDAGToDAG.cpp - KlaussCPU DAG->DAG Instruction Selector ===//
//
// KlaussCPU LLVM backend — DAG-to-DAG instruction selection pass.
//
// Instruction selection is the tablegen-generated SelectCode() (from
// KlaussCPUGenDAGISel.inc), driven by the isel patterns in
// KlaussCPUInstrInfo.td.  This file holds the pass scaffolding, the
// ComplexPattern hooks (SelectAddrFI/SelectAddrFIMem for frame addresses;
// selectShiftMask, selectDup32, selectBitFieldExtract and
// selectFieldShiftPair for shifts, 32-bit rotates and bit fields), the
// inline-asm memory operand hook and the nodes Select() handles itself:
//   BR_CC, KlaussCPUISD::SELECT_CC, SETCC: they pick the 32-bit W compare by
//     looking at how each operand was extended and whether stripping the
//     extension removes an instruction (SETCC otherwise falls back to the
//     patterns).  BR_CC also glues the compare (which defines only FLAGS) to
//     its conditional jump; as a pattern that would need a FLAGS-producing
//     compare node, and the W-compare choice would still be C++, so these
//     nodes stay here.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUTargetMachine.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/CodeGen/FunctionLoweringInfo.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/CodeGen/SelectionDAGNodes.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/KnownBits.h"
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

/// Is every use of Ext a compare operand of a BR_CC, SELECT_CC or SETCC node
/// that compares LHS with RHS under CC?  tryW32Compare strips Ext from each of
/// them alike, so Ext dies.  Several selects on one condition (which the
/// custom inserter then expands with one shared compare) are the usual case.
static bool onlyFeedsCompare(SDNode *Ext, SDValue LHS, SDValue RHS,
                             ISD::CondCode CC) {
  for (const SDUse &U : Ext->uses()) {
    const SDNode *User = U.getUser();
    unsigned LHSNo;
    if (User->getOpcode() == KlaussCPUISD::SELECT_CC ||
        User->getOpcode() == ISD::SETCC)
      LHSNo = 0;
    else if (User->getOpcode() == ISD::BR_CC)
      LHSNo = 2;
    else
      return false;
    unsigned CCNo = LHSNo == 0 ? 2 : 1;
    if ((U.getOperandNo() != LHSNo && U.getOperandNo() != LHSNo + 1) ||
        User->getOperand(LHSNo) != LHS || User->getOperand(LHSNo + 1) != RHS ||
        cast<CondCodeSDNode>(User->getOperand(CCNo))->get() != CC)
      return false;
  }
  return true;
}

/// ISA v3 D2: can this compare use the 32-bit W form?  If so, strip the
/// explicit extensions from LHS/RHS in place (RHS may stay a constant) and
/// return true.  Sign-extension preserves both signed and unsigned 32-bit
/// order, so a sext pair works for any condition; a zext pair only for
/// EQ/NE and the unsigned conditions.  Only used when an extension
/// instruction actually disappears: it feeds nothing but compares like this
/// one.  (A branch on the 64-bit compare of two registers or with a simm4
/// fuses into an FBR, which the W compare cannot, so with one extension gone
/// the instruction count is even there; the W form still wins on the
/// emulator corpus, as it keeps the unextended value's register.)
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
  bool Saves = (L.Stripped && onlyFeedsCompare(L.Node, LHS, RHS, CC)) ||
               (!RC && R.Stripped && onlyFeedsCompare(R.Node, LHS, RHS, CC));
  if (K == Ext32::None || !Saves)
    return false;
  LHS = L.Inner;
  if (!RC)
    RHS = R.Inner;
  return true;
}

// The boolean W compare (register or immediate form) for CC, or 0.
static unsigned getBoolCompareW(ISD::CondCode CC, bool Imm) {
  switch (CC) {
  case ISD::SETEQ:  return Imm ? KlaussCPU::CMPEQVW  : KlaussCPU::CMPEQRW;
  case ISD::SETNE:  return Imm ? KlaussCPU::CMPNEVW  : KlaussCPU::CMPNERW;
  case ISD::SETLT:  return Imm ? KlaussCPU::CMPLTVW  : KlaussCPU::CMPLTRW;
  case ISD::SETGE:  return Imm ? KlaussCPU::CMPGEVW  : KlaussCPU::CMPGERW;
  case ISD::SETLE:  return Imm ? KlaussCPU::CMPLEVW  : KlaussCPU::CMPLERW;
  case ISD::SETGT:  return Imm ? KlaussCPU::CMPGTVW  : KlaussCPU::CMPGTRW;
  case ISD::SETULT: return Imm ? KlaussCPU::CMPULTVW : KlaussCPU::CMPULTRW;
  case ISD::SETUGE: return Imm ? KlaussCPU::CMPUGEVW : KlaussCPU::CMPUGERW;
  case ISD::SETULE: return Imm ? KlaussCPU::CMPULEVW : KlaussCPU::CMPULERW;
  case ISD::SETUGT: return Imm ? KlaussCPU::CMPUGTVW : KlaussCPU::CMPUGTRW;
  default:          return 0;
  }
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

  bool runOnMachineFunction(MachineFunction &MF) override {
    LocalDist.clear();
    CycleBlocks.reset();
    return SelectionDAGISel::runOnMachineFunction(MF);
  }

  void PreprocessISelDAG() override;
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
  bool frameOffsetFitsShortForm(int FI, int64_t C, unsigned Scale);

  bool selectShiftMask(SDValue N, SDValue &ShAmt);
  bool selectDup32(SDValue N, SDValue &Hi, SDValue &Lo);
  bool selectBitFieldExtract(SDValue N, SDValue &Src, SDValue &Field);
  bool selectFieldShiftPair(SDValue N, SDValue &Src, SDValue &Shl,
                            SDValue &Srl);
  bool preferMaskShiftPair() const;

  bool SelectInlineAsmMemoryOperand(const SDValue &Op,
                                    InlineAsm::ConstraintCode ConstraintID,
                                    std::vector<SDValue> &OutOps) override;

  // Auto-generated tablegen instruction selection tables.
#include "KlaussCPUGenDAGISel.inc"

private:
  // frameOffsetFitsShortForm: LocalDist[I] bounds how far below the frame
  // register local I can start.  Filled lazily, once per function.
  SmallVector<int64_t, 0> LocalDist;
  // preferMaskShiftPair: the IR blocks on a CFG cycle.  Filled lazily, once
  // per function.
  mutable std::optional<SmallPtrSet<const BasicBlock *, 16>> CycleBlocks;
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

// No 64-bit load reads an unaligned doubleword: the A=1 forms (LDIDX64,
// MEMGET64) align the EA down to 8 and the A=0 ones (LDIDX64R, MEMREADR) must
// be given an aligned one (CPU_ARCHITECTURE.md 8.3), so the i64 load patterns
// take only 8-aligned loads (load_a8).  LowerLOAD splits an under-aligned i64
// load at legalization (allowsMisalignedMemoryAccesses lets the combiner form
// one), and the post-legalize combiner legalizes every node it creates, so
// none is expected here.  This is a defensive backstop: should one reach isel
// anyway, it is split, after the last combine, into the same two 32-bit loads
// (MEMGET32 below 4-byte alignment, LDIDX32 at 4) + SHLV + ORR, rather than
// fail to select.  -klausscpu-split-load64-at-legalize=false sends every such
// non-volatile load here, which is how unaligned-load-backstop.ll tests it.
void KlaussCPUDAGToDAGISel::PreprocessISelDAG() {
  SmallVector<LoadSDNode *, 8> Loads;
  for (SDNode &N : CurDAG->allnodes())
    if (auto *LD = dyn_cast<LoadSDNode>(&N);
        LD && LD->getValueType(0) == MVT::i64 && ISD::isNormalLoad(LD) &&
        LD->getAlign() < Align(8))
      Loads.push_back(LD);
  if (Loads.empty())
    return;
  // The replacements are new nodes, so no user of a load is merged with an
  // existing node (and deleted) when its operand changes.
  for (LoadSDNode *LD : Loads) {
    auto S = KlaussCPUTargetLowering::splitUnalignedLoad64(LD, *CurDAG);
    SDValue Val = KlaussCPUTargetLowering::joinHalves(S, SDLoc(LD), *CurDAG);
    LLVM_DEBUG(dbgs() << "Splitting under-aligned i64 load: ";
               LD->dump(CurDAG));
    CurDAG->ReplaceAllUsesOfValueWith(SDValue(LD, 0), Val);
    CurDAG->ReplaceAllUsesOfValueWith(SDValue(LD, 1), S.Chain);
  }
  CurDAG->RemoveDeadNodes();
}

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

  // ---- ISD::SETCC → ISA v3 D2 boolean W compare -------------------------
  // CMPccRW / CMPccVW when stripping the operands' 32-bit extensions removes
  // an instruction; otherwise the patterns pick CMPccR / CMPccV.
  if (N->getOpcode() == ISD::SETCC &&
      N->getOperand(0).getValueType() == MVT::i64) {
    SDValue LHS = N->getOperand(0);
    SDValue RHS = N->getOperand(1);
    ISD::CondCode CC = cast<CondCodeSDNode>(N->getOperand(2))->get();
    if (unsigned Opc = getBoolCompareW(CC, isa<ConstantSDNode>(RHS));
        Opc && tryW32Compare(CC, LHS, RHS)) {
      SDLoc DL(N);
      if (auto *C = dyn_cast<ConstantSDNode>(RHS))
        RHS = CurDAG->getTargetConstant(
            static_cast<int32_t>(C->getZExtValue()), DL, MVT::i64);
      ReplaceNode(N, CurDAG->getMachineNode(Opc, DL, N->getValueType(0), LHS,
                                            RHS));
      return;
    }
  }

  // Delegate everything else to the tablegen-generated SelectCode().
  SelectCode(N);
}

// A constant is folded onto a frame index only while slot offset + constant
// is sure to fit in simm32: -512 MiB <= constant < 512 MiB, four times the
// address space, so only a frame over 1.5 GiB could overflow
// (eliminateFrameIndex copes with that too, except at an inline asm).
static bool isFrameIndexOffset(int64_t C) { return isInt<30>(C); }

// Would frame object FI + C be in reach of the 1-word short form (a signed
// 8-bit offset scaled by the access size, unscaled for ADDI) once the frame
// is laid out?  An incoming-argument slot's offset is already known.  A
// local's is not, but PEI places the callee-saved slots (at most R4-R7)
// nearest the frame register, then the locals in index order and the spill
// slots, which are created later, below them: the local is no further away
// than the callee-saved area plus the size of every local up to and
// including it.  These running sums are computed once per function (an
// object created or realigned later in isel is seen as it was then); this is
// only a size estimate, and any offset is correct.
bool KlaussCPUDAGToDAGISel::frameOffsetFitsShortForm(int FI, int64_t C,
                                                     unsigned Scale) {
  const MachineFrameInfo &MFI = MF->getFrameInfo();
  int64_t Lo = -128 * int64_t(Scale), Hi = 127 * int64_t(Scale);
  if (MFI.isFixedObjectIndex(FI)) {
    // getFrameIndexReference: fixed objects are R15 + 8 + their offset.
    int64_t Off = MFI.getObjectOffset(FI) + 8 + C;
    return Off >= Lo && Off <= Hi;
  }
  for (int I = LocalDist.size(); I <= FI; ++I) {
    int64_t Dist = LocalDist.empty() ? 4 * 8 : LocalDist.back();
    if (!MFI.isDeadObjectIndex(I) && !MFI.isVariableSizedObjectIndex(I))
      Dist += MFI.getObjectSize(I) + MFI.getObjectAlign(I).value() - 1;
    LocalDist.push_back(Dist);
  }
  // The slot starts somewhere in [R15 - Dist, R15 - 1].
  int64_t Dist = LocalDist[FI];
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
        if (Accesses > 2)
          return false;
      }
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

// shamt: the amount register of SHLR/SHRR/SARR/ROLR/RORR_R.  The CPU uses
// only its bits [5:0], so whatever leaves those bits alone is dropped, as in
// RISCVDAGToDAGISel::selectShiftMask.  A shift by 64 or more is poison and a
// rotate is modulo 64, so the result is the same for every amount the IR
// allows.
//
// An `and` that keeps bits [5:0] (the `& 63` of a rotate or funnel shift,
// the zero-extension of a promoted i32 amount) is dropped.
// SimplifyDemandedBits may have cleared mask bits that are known zero anyway.
static SDValue stripShiftMask(SDValue V, SelectionDAG &DAG) {
  if (V.getOpcode() != ISD::AND || !isa<ConstantSDNode>(V.getOperand(1)))
    return V;
  const APInt ShMask(64, 63);
  const APInt &AndMask = V.getConstantOperandAPInt(1);
  if (!ShMask.isSubsetOf(AndMask) &&
      !ShMask.isSubsetOf(AndMask | DAG.computeKnownBits(V.getOperand(0)).Zero))
    return V;
  return V.getOperand(0);
}

// X + 64k, X - 64k, X | 64k and X ^ 64k have the low bits of X.
static bool isShamtNoOp(SDValue V) {
  unsigned Opc = V.getOpcode();
  return (Opc == ISD::ADD || Opc == ISD::SUB || Opc == ISD::OR ||
          Opc == ISD::XOR) &&
         isa<ConstantSDNode>(V.getOperand(1)) &&
         V.getConstantOperandVal(1) % 64 == 0;
}

bool KlaussCPUDAGToDAGISel::selectShiftMask(SDValue N, SDValue &ShAmt) {
  // The low bits of a sum, difference, OR or XOR depend only on the low bits
  // of its operands, so inner masks go too.  X + 64k, X - 64k, X | 64k and
  // X ^ 64k shift by X (the >= 64 arm of a 128-bit shift is (X | 64) - 64);
  // 64k - X by -X and 64k-1 - X (or X ^ 63) by ~X: one unary op instead of a
  // constant and a SUBR.
  ShAmt = stripShiftMask(N, *CurDAG);
  while (isShamtNoOp(ShAmt))
    ShAmt = stripShiftMask(ShAmt.getOperand(0), *CurDAG);
  unsigned NewOpc = 0;
  SDValue X;
  unsigned Opc = ShAmt.getOpcode();
  if (Opc == ISD::SUB && isa<ConstantSDNode>(ShAmt.getOperand(0))) {
    uint64_t Imm = ShAmt.getConstantOperandVal(0) % 64;
    if (Imm == 0 || Imm == 63) {
      NewOpc = Imm == 0 ? KlaussCPU::NEGR : KlaussCPU::NOTR;
      X = ShAmt.getOperand(1);
    }
  } else if (Opc == ISD::XOR && isa<ConstantSDNode>(ShAmt.getOperand(1)) &&
             ShAmt.getConstantOperandVal(1) % 64 == 63) {
    NewOpc = KlaussCPU::NOTR;
    X = ShAmt.getOperand(0);
  }
  if (NewOpc)
    ShAmt = SDValue(CurDAG->getMachineNode(NewOpc, SDLoc(ShAmt), MVT::i64,
                                           stripShiftMask(X, *CurDAG)),
                    0);
  return true;
}

// Dup32: the two halves of ORR (SHLV Hi, 32), Lo, which copies N[31:0] into
// both halves of a register for a 32-bit rotate.  Lo must be N zero-extended
// from 32 bits: an explicit `and N', 0xFFFFFFFF` (then Hi is N', so the
// SHLV does not wait for the ZEXTW), N itself when its upper half is known
// zero or it is a load of at most 32 bits (every one of which zero-extends,
// extloads included: see isZExtFree), or a new ZEXTW.
bool KlaussCPUDAGToDAGISel::selectDup32(SDValue N, SDValue &Hi, SDValue &Lo) {
  Hi = Lo = N;
  if (N.getOpcode() == ISD::AND && isa<ConstantSDNode>(N.getOperand(1)) &&
      N.getConstantOperandVal(1) == 0xFFFFFFFFu) {
    Hi = N.getOperand(0);
    return true;
  }
  if (auto *LD = dyn_cast<LoadSDNode>(N);
      LD && LD->getExtensionType() != ISD::SEXTLOAD &&
      LD->getMemoryVT().getSizeInBits() <= 32)
    return true;
  if (!CurDAG->MaskedValueIsZero(N, APInt::getHighBitsSet(64, 32)))
    Lo = SDValue(
        CurDAG->getMachineNode(KlaussCPU::ZEXTW, SDLoc(N), MVT::i64, N), 0);
  return true;
}

// BitFieldExtract: BEXTR rd, rs, {len[12:8], start[4:0]} computes
// rd = (rs >> start) & (2^len - 1) for start, len in [0, 31] (the CPU's
// field descriptor; pipeline_core.sv U_BEXTR).  Matched for
//   (and (srl x, start), 2^len - 1)
//   (srl (and x, M), start)  with M >> start = 2^len - 1 (a zero-extended
//                            i32 shift; the bits of M below start are
//                            shifted out)
// with start >= 1 (a mask alone is one ANDV).  The inner node must have no
// other use, or it stays and BEXTR saves nothing.
bool KlaussCPUDAGToDAGISel::selectBitFieldExtract(SDValue N, SDValue &Src,
                                                  SDValue &Field) {
  unsigned Opc = N.getOpcode();
  if ((Opc != ISD::AND && Opc != ISD::SRL) ||
      !isa<ConstantSDNode>(N.getOperand(1)))
    return false;
  SDValue Inner = N.getOperand(0);
  if (Inner.getOpcode() != (Opc == ISD::AND ? ISD::SRL : ISD::AND) ||
      !Inner.hasOneUse() || !isa<ConstantSDNode>(Inner.getOperand(1)))
    return false;
  uint64_t Start, Mask;
  if (Opc == ISD::AND) {
    Start = Inner.getConstantOperandVal(1);
    Mask = N.getConstantOperandVal(1);
  } else {
    Start = N.getConstantOperandVal(1);
    if (Start >= 64)
      return false;
    Mask = Inner.getConstantOperandVal(1) >> Start;
  }
  if (Start < 1 || Start > 31 || !isMask_64(Mask))
    return false;
  unsigned Len = llvm::popcount(Mask);
  if (Len > 31)
    return false;
  Src = Inner.getOperand(0);
  Field = CurDAG->getTargetConstant((Len << 8) | Start, SDLoc(N), MVT::i64);
  return true;
}

// FieldShiftPair: (and (srl x, start), 2^len - 1) that BEXTR cannot take
// (start or len above 31) and whose mask is not 1 word (ANDV with a uimm8,
// ZEXTB/H/W): (x << (64 - start - len)) >> (64 - len), two 1-word shifts
// instead of SHRV and a 2-word ANDV, or a SETR64 + ANDR, e.g. the exponent
// of a double, (x >> 52) & 0x7FF.
bool KlaussCPUDAGToDAGISel::selectFieldShiftPair(SDValue N, SDValue &Src,
                                                 SDValue &Shl, SDValue &Srl) {
  if (N.getOpcode() != ISD::AND || !isa<ConstantSDNode>(N.getOperand(1)))
    return false;
  SDValue Sh = N.getOperand(0);
  if (Sh.getOpcode() != ISD::SRL || !Sh.hasOneUse() ||
      !isa<ConstantSDNode>(Sh.getOperand(1)))
    return false;
  uint64_t Start = Sh.getConstantOperandVal(1);
  uint64_t Mask = N.getConstantOperandVal(1);
  if (!isMask_64(Mask) || Mask <= 0xFF || Mask == 0xFFFF ||
      Mask == 0xFFFFFFFF)
    return false;
  unsigned Len = llvm::popcount(Mask);
  if ((Start >= 1 && Start <= 31 && Len <= 31) || Start + Len >= 64)
    return false;
  SDLoc DL(N);
  Src = Sh.getOperand(0);
  Shl = CurDAG->getTargetConstant(64 - Start - Len, DL, MVT::i64);
  Srl = CurDAG->getTargetConstant(64 - Len, DL, MVT::i64);
  return true;
}

// highmask64_imm / lowmask64_imm: an AND with a 64-bit mask is a shift pair
// (2 words) rather than SETR64 + ANDR (4 words, the same count) unless the
// block is on a CFG cycle in a function optimised for speed.  There
// MachineLICM hoists the SETR64 out of the loop, which leaves one ANDR per
// iteration instead of two shifts.  At -O0 nothing is hoisted, and with
// optsize the 2 words win.
bool KlaussCPUDAGToDAGISel::preferMaskShiftPair() const {
  const Function &F = CurDAG->getMachineFunction().getFunction();
  if (OptLevel == CodeGenOptLevel::None || F.hasOptSize())
    return true;
  const BasicBlock *BB = FuncInfo->MBB->getBasicBlock();
  if (!BB)
    return true;
  if (!CycleBlocks) {
    CycleBlocks.emplace();
    for (scc_iterator<const Function *> I = scc_begin(&F); !I.isAtEnd(); ++I)
      if (I.hasCycle())
        CycleBlocks->insert(I->begin(), I->end());
  }
  return !CycleBlocks->contains(BB);
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
