//===-- KlaussCPUISelDAGToDAG.cpp - KlaussCPU DAG->DAG Instruction Selector ===//
//
// KlaussCPU LLVM backend — DAG-to-DAG instruction selection pass.
//
// The bulk of instruction selection is handled by the tablegen-generated
// SelectCode() (from KlaussCPUGenDAGISel.inc), which uses the isel patterns
// defined in KlaussCPUInstrInfo.td.  This file handles the pass scaffolding
// and any nodes that cannot be covered by a tablegen pattern alone.
//
// Manual selections in Select():
//   ISD::CALLSEQ_START → ADJCALLSTACKDOWN (pseudo, eliminated by PEI)
//   ISD::CALLSEQ_END   → ADJCALLSTACKUP   (pseudo, eliminated by PEI)
//
// Reason: KlaussCPU has no i32 register class, so the standard tablegen
// approach of matching (callseq_start timm:$n, timm:$m) would fail the
// type-set check.  Matching in C++ avoids the issue.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUTargetMachine.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/CodeGen/SelectionDAGNodes.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"

// KlaussCPU:: opcode enum (ADJCALLSTACKDOWN, ADJCALLSTACKUP, etc.) is visible
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

//===----------------------------------------------------------------------===//
// KlaussCPU-specific DAG-to-DAG instruction selector.
//===----------------------------------------------------------------------===//
namespace {

class KlaussCPUDAGToDAGISel : public SelectionDAGISel {
public:
  explicit KlaussCPUDAGToDAGISel(KlaussCPUTargetMachine &TM)
      : SelectionDAGISel(TM) {}

  void Select(SDNode *N) override;

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

  // ---- ISD::FrameIndex → ADDI(TargetFrameIndex, 0) ------------------------
  // A bare TargetFrameIndex (TFI) is only a placeholder operand inside a
  // machine instruction; it is NOT a value node that InstrEmitter can assign
  // to a virtual register.  If a FrameIndex is used in a CopyToReg (e.g.
  // passing a stack-local array as a function argument) or in an ADD (variable-
  // indexed stack access), InstrEmitter::getVR would assert.
  //
  // Emit ADDI rd, <TFI>, 0.  eliminateFrameIndex sees TFI at the rs slot
  // (FIOperandNum=1) with imm=0 at FIOperandNum+1, and rewrites to:
  //   ADDI rd, R15, frame_slot_offset   (one instruction, replaces SETR+ADDR)
  if (N->getOpcode() == ISD::FrameIndex) {
    int FI = cast<FrameIndexSDNode>(N)->getIndex();
    SDLoc DL(N);
    SDValue TFI = CurDAG->getTargetFrameIndex(FI, MVT::i64);
    SDValue Off0 = CurDAG->getTargetConstant(0, DL, MVT::i64);
    SDNode *Res = CurDAG->getMachineNode(KlaussCPU::ADDI, DL, MVT::i64,
                                         {TFI, Off0});
    ReplaceNode(N, Res);
    return;
  }

  // TargetFrameIndex is a legal operand placeholder; no instruction needed.
  if (N->getOpcode() == ISD::TargetFrameIndex)
    return;

  // ---- KlaussCPUISD::ADDR → SETR (non-PIC global address) -------------------
  // LowerGlobalAddress wraps TGA in ADDR (non-PIC) or LEAPC (PIC).
  // Here we peel off ADDR and emit SETR with the enclosed symbol (ABS32 fixup).
  if (N->getOpcode() == KlaussCPUISD::ADDR) {
    SDValue Sym = N->getOperand(0);
    SDLoc DL(N);
    SDNode *Res = CurDAG->getMachineNode(KlaussCPU::SETR, DL, MVT::i64, Sym);
    ReplaceNode(N, Res);
    return;
  }

  // ---- KlaussCPUISD::LEAPC → LEAPC (PIC global address) --------------------
  // LowerGlobalAddress uses LEAPC in PIC mode.  Emit LEAPC machine instruction
  // with the enclosed symbol — encoder emits FK_KlaussCPU_PCREL32 fixup.
  if (N->getOpcode() == KlaussCPUISD::LEAPC) {
    SDValue Sym = N->getOperand(0);
    SDLoc DL(N);
    SDNode *Res = CurDAG->getMachineNode(KlaussCPU::LEAPC, DL, MVT::i64, Sym);
    ReplaceNode(N, Res);
    return;
  }

  // ---- Frame-slot LOAD: (load (FrameIndex|TargetFrameIndex)) → LDIDX64/32 ---
  // The 0 offset is a placeholder; eliminateFrameIndex will fill in the real
  // frame offset (R15 + slot_offset).  Machine node order: [base, offset, chain].
  //
  // We check for both ISD::FrameIndex and ISD::TargetFrameIndex because the
  // node ordering during selection is not strictly guaranteed: the LOAD may be
  // visited before the FrameIndex handler has converted the pointer.
  //
  // i8/i16 frame-slot loads require address materialisation via a scratch
  // register (register scavenging) which is not yet implemented.  Those cases
  // fall through to SelectCode and will produce a "Cannot select" error.
  // Use -O1 or higher to eliminate frame-local i8/i16 allocas via mem2reg.
  if (N->getOpcode() == ISD::LOAD) {
    auto *LN = cast<LoadSDNode>(N);
    SDValue Ptr = LN->getBasePtr();
    // Normalise FrameIndex → TargetFrameIndex if not already done.
    if (Ptr.getOpcode() == ISD::FrameIndex) {
      int FI = cast<FrameIndexSDNode>(Ptr.getNode())->getIndex();
      Ptr = CurDAG->getTargetFrameIndex(FI, MVT::i64);
    }
    if (Ptr.getOpcode() == ISD::TargetFrameIndex) {
      SDLoc DL(N);
      SDValue Off = CurDAG->getTargetConstant(0, DL, MVT::i64);

      // 64-bit non-extending frame-slot load → LDIDX64.
      if (LN->getExtensionType() == ISD::NON_EXTLOAD &&
          LN->getMemoryVT() == MVT::i64) {
        SDValue Ops[] = {Ptr, Off, LN->getChain()};
        SDNode *Res = CurDAG->getMachineNode(KlaussCPU::LDIDX64, DL,
                                              CurDAG->getVTList(MVT::i64,
                                                                MVT::Other),
                                              Ops);
        ReplaceUses(N, Res);
        CurDAG->RemoveDeadNode(N);
        return;
      }

      // 32-bit zero/any-extending frame-slot load → LDIDX32 (zero-extends to i64).
      if (LN->getMemoryVT() == MVT::i32 &&
          (LN->getExtensionType() == ISD::ZEXTLOAD ||
           LN->getExtensionType() == ISD::EXTLOAD)) {
        SDValue Ops[] = {Ptr, Off, LN->getChain()};
        SDNode *Res = CurDAG->getMachineNode(KlaussCPU::LDIDX32, DL,
                                              CurDAG->getVTList(MVT::i64,
                                                                MVT::Other),
                                              Ops);
        ReplaceUses(N, Res);
        CurDAG->RemoveDeadNode(N);
        return;
      }

      // 8/16-bit zero/any-extending frame-slot loads → LDIDX8/16.
      if (LN->getExtensionType() == ISD::ZEXTLOAD ||
          LN->getExtensionType() == ISD::EXTLOAD) {
        unsigned LoadOpc = 0;
        if (LN->getMemoryVT() == MVT::i8)
          LoadOpc = KlaussCPU::LDIDX8;
        else if (LN->getMemoryVT() == MVT::i16)
          LoadOpc = KlaussCPU::LDIDX16;
        if (LoadOpc) {
          SDValue Ops[] = {Ptr, Off, LN->getChain()};
          SDNode *Res = CurDAG->getMachineNode(LoadOpc, DL,
                                                CurDAG->getVTList(MVT::i64,
                                                                  MVT::Other),
                                                Ops);
          ReplaceUses(N, Res);
          CurDAG->RemoveDeadNode(N);
          return;
        }
      }

      // 8/16-bit sign-extending frame-slot loads → LDIDX8_S/LDIDX16_S.
      if (LN->getExtensionType() == ISD::SEXTLOAD) {
        unsigned LoadOpc = 0;
        if (LN->getMemoryVT() == MVT::i8)
          LoadOpc = KlaussCPU::LDIDX8_S;
        else if (LN->getMemoryVT() == MVT::i16)
          LoadOpc = KlaussCPU::LDIDX16_S;
        if (LoadOpc) {
          SDValue Ops[] = {Ptr, Off, LN->getChain()};
          SDNode *Res = CurDAG->getMachineNode(LoadOpc, DL,
                                                CurDAG->getVTList(MVT::i64,
                                                                  MVT::Other),
                                                Ops);
          ReplaceUses(N, Res);
          CurDAG->RemoveDeadNode(N);
          return;
        }
      }
    }
  }

  // ---- Frame-slot STORE: (store val, (FrameIndex|TargetFrameIndex)) → STIDX64/32 ---
  // Machine node order: [data, base, offset, chain].
  // We check for both FrameIndex and TargetFrameIndex for the same reason as LOAD.
  if (N->getOpcode() == ISD::STORE) {
    auto *SN = cast<StoreSDNode>(N);
    SDValue Ptr = SN->getBasePtr();
    // Normalise FrameIndex → TargetFrameIndex if not already done.
    if (Ptr.getOpcode() == ISD::FrameIndex) {
      int FI = cast<FrameIndexSDNode>(Ptr.getNode())->getIndex();
      Ptr = CurDAG->getTargetFrameIndex(FI, MVT::i64);
    }
    if (Ptr.getOpcode() == ISD::TargetFrameIndex) {
      SDLoc DL(N);
      SDValue Off = CurDAG->getTargetConstant(0, DL, MVT::i64);

      // 64-bit non-truncating frame-slot store → STIDX64.
      if (!SN->isTruncatingStore() && SN->getMemoryVT() == MVT::i64) {
        SDValue Ops[] = {SN->getValue(), Ptr, Off, SN->getChain()};
        SDNode *Res = CurDAG->getMachineNode(KlaussCPU::STIDX64, DL,
                                              CurDAG->getVTList(MVT::Other),
                                              Ops);
        ReplaceNode(N, Res);
        return;
      }

      // 32-bit truncating frame-slot store → STIDX32.
      if (SN->isTruncatingStore() && SN->getMemoryVT() == MVT::i32) {
        SDValue Ops[] = {SN->getValue(), Ptr, Off, SN->getChain()};
        SDNode *Res = CurDAG->getMachineNode(KlaussCPU::STIDX32, DL,
                                              CurDAG->getVTList(MVT::Other),
                                              Ops);
        ReplaceNode(N, Res);
        return;
      }

      // 8/16-bit frame-slot stores → STIDX8/16 (base+offset RRV format).
      // eliminateFrameIndex rewrites Ptr→R15 and Off→slot_offset normally.
      if (SN->isTruncatingStore()) {
        unsigned StoreOpc = 0;
        if (SN->getMemoryVT() == MVT::i8)
          StoreOpc = KlaussCPU::STIDX8;
        else if (SN->getMemoryVT() == MVT::i16)
          StoreOpc = KlaussCPU::STIDX16;
        if (StoreOpc) {
          SDValue Ops[] = {SN->getValue(), Ptr, Off, SN->getChain()};
          SDNode *Res = CurDAG->getMachineNode(StoreOpc, DL,
                                                CurDAG->getVTList(MVT::Other),
                                                Ops);
          ReplaceNode(N, Res);
          return;
        }
      }
    }
  }

  // Lower ISD::CALLSEQ_START/END to the ADJCALLSTACK pseudos manually.
  // We do this in C++ rather than tablegen patterns because KlaussCPU has no
  // i32 register class, which would cause tablegen type-set inference to fail
  // on the (callseq_start timm:$n, timm:$m) pattern.
  if (N->getOpcode() == ISD::CALLSEQ_START ||
      N->getOpcode() == ISD::CALLSEQ_END) {
    unsigned Opc = (N->getOpcode() == ISD::CALLSEQ_START)
                       ? KlaussCPU::ADJCALLSTACKDOWN
                       : KlaussCPU::ADJCALLSTACKUP;
    SDLoc DL(N);
    // CALLSEQ_START/END operands: chain(0), size(1), zero(2) [, glue(3) for END]
    //
    // InstrEmitter::countOperands strips chain and glue from the TAIL, so the
    // machine-node operand order must be:
    //   [imm1, imm2, chain]          — for START
    //   [imm1, imm2, chain, glue]    — for END with glue
    // Putting chain first would make countOperands count it as an extra explicit
    // operand, exceeding the two that ADJCALLSTACKDOWN/UP declare.
    auto *SizeNode = cast<ConstantSDNode>(N->getOperand(1));
    auto *ZeroNode = cast<ConstantSDNode>(N->getOperand(2));
    SDValue Imm1 = CurDAG->getTargetConstant(SizeNode->getZExtValue(), DL, MVT::i32);
    SDValue Imm2 = CurDAG->getTargetConstant(ZeroNode->getZExtValue(), DL, MVT::i32);
    // CALLSEQ_END carries an optional glue input at operand index 3.
    if (N->getOpcode() == ISD::CALLSEQ_END && N->getNumOperands() > 3) {
      SDValue GlueOps[] = {Imm1, Imm2, N->getOperand(0), N->getOperand(3)};
      CurDAG->SelectNodeTo(N, Opc, MVT::Other, MVT::Glue, GlueOps);
    } else {
      SDValue Ops[] = {Imm1, Imm2, N->getOperand(0)};
      CurDAG->SelectNodeTo(N, Opc, MVT::Other, MVT::Glue, Ops);
    }
    return;
  }

  // ---- ISD::Constant: 64-bit values that don't fit in simm32 --------------
  // SETR covers [-2^31, 2^31-1].  Larger values use SETR64: a single
  // 12-byte instruction rd = (hi32 << 32) | lo32.
  if (N->getOpcode() == ISD::Constant) {
    auto *C = cast<ConstantSDNode>(N);
    int64_t Val = C->getSExtValue();
    if (!isInt<32>(Val)) {
      SDLoc DL(N);
      uint64_t UVal = static_cast<uint64_t>(Val);
      SDValue LoImm = CurDAG->getTargetConstant(
          static_cast<int32_t>(static_cast<uint32_t>(UVal)), DL, MVT::i64);
      SDValue HiImm = CurDAG->getTargetConstant(
          static_cast<int32_t>(UVal >> 32), DL, MVT::i64);
      SDNode *Res = CurDAG->getMachineNode(KlaussCPU::SETR64, DL, MVT::i64,
                                            {LoImm, HiImm});
      ReplaceNode(N, Res);
      return;
    }
    // simm32 constants: fall through to SelectCode (matched by SETR pattern).
  }

  // ---- ISD::BR → JMP / JMPREL (unconditional branch) ----------------------
  // ISD::BR: (chain, dest_bb)
  if (N->getOpcode() == ISD::BR) {
    SDValue Chain = N->getOperand(0);
    SDValue Dest  = N->getOperand(1);
    SDLoc DL(N);
    unsigned Opc = TM.isPositionIndependent() ? KlaussCPU::JMPREL : KlaussCPU::JMP;
    SDValue Ops[] = {Dest, Chain};
    SDNode *Res = CurDAG->getMachineNode(Opc, DL, MVT::Other, Ops);
    ReplaceNode(N, Res);
    return;
  }

  // ---- ISD::BRIND → JMPR_R (indirect register branch) --------------------
  // ISD::BRIND: (chain, target_reg)
  // Used for computed gotos and as the primitive emitted by BR_JT expansion.
  if (N->getOpcode() == ISD::BRIND) {
    SDValue Chain  = N->getOperand(0);
    SDValue Target = N->getOperand(1);
    SDLoc DL(N);
    SDNode *Res = CurDAG->getMachineNode(KlaussCPU::JMPR_R, DL,
                                         MVT::Other, {Target, Chain});
    ReplaceNode(N, Res);
    return;
  }

  // ---- ISD::BR_CC → CMPRR_I/CMPRV_I + conditional JMP -------------------
  // ISD::BR_CC: (chain, condcode, lhs, rhs, dest_bb)
  //
  // The compare instruction has no output register (it only sets processor
  // flags).  We model the dependency via a chain output from the compare,
  // which is consumed as the chain input to the branch.
  //
  // CMPRV_I accepts a signed-32-bit immediate RHS directly.
  // CMPRR_I requires both operands to be in registers.
  // Large constants (> INT32_MAX) must be in a register at this point; they
  // are materialized by SETR. 64-bit constants > INT_MAX need SETR64 (step 13).
  if (N->getOpcode() == ISD::BR_CC) {
    ISD::CondCode CC = cast<CondCodeSDNode>(N->getOperand(1))->get();
    SDValue Chain    = N->getOperand(0);
    SDValue LHS      = N->getOperand(2);
    SDValue RHS      = N->getOperand(3);
    SDValue Dest     = N->getOperand(4);
    SDLoc DL(N);

    // Emit the compare.
    SDNode *CmpNode;
    if (auto *C = dyn_cast<ConstantSDNode>(RHS);
        C && isInt<32>(C->getSExtValue())) {
      // Constant RHS that fits in a signed 32-bit field → CMPRV_I.
      SDValue Imm  = CurDAG->getTargetConstant(C->getSExtValue(), DL, MVT::i64);
      SDValue Ops[] = {LHS, Imm, Chain};
      CmpNode = CurDAG->getMachineNode(KlaussCPU::CMPRV_I, DL, MVT::Other, Ops);
    } else {
      // Register-register compare.
      SDValue Ops[] = {LHS, RHS, Chain};
      CmpNode = CurDAG->getMachineNode(KlaussCPU::CMPRR_I, DL, MVT::Other, Ops);
    }

    // Map ISD::CondCode to the KlaussCPU conditional jump opcode.
    // CMPRR/CMPRV set: equal / less (signed) / ult (unsigned) / sign.
    // PIC mode uses the PC-relative REL variants; non-PIC uses absolute.
    bool PIC = TM.isPositionIndependent();
    unsigned JmpOpc;
    switch (CC) {
    case ISD::SETEQ:   JmpOpc = PIC ? KlaussCPU::JMPEREL    : KlaussCPU::JMPE;    break;
    case ISD::SETNE:   JmpOpc = PIC ? KlaussCPU::JMPNEREL   : KlaussCPU::JMPNE;   break;
    case ISD::SETLT:   JmpOpc = PIC ? KlaussCPU::JMPLTREL   : KlaussCPU::JMPLT;   break;
    case ISD::SETLE:   JmpOpc = PIC ? KlaussCPU::JMPLEREL   : KlaussCPU::JMPLE;   break;
    case ISD::SETGT:   JmpOpc = PIC ? KlaussCPU::JMPGTREL   : KlaussCPU::JMPGT;   break;
    case ISD::SETGE:   JmpOpc = PIC ? KlaussCPU::JMPGEREL   : KlaussCPU::JMPGE;   break;
    case ISD::SETULT:  JmpOpc = PIC ? KlaussCPU::JMPULTREL  : KlaussCPU::JMPULT;  break;
    case ISD::SETULE:  JmpOpc = PIC ? KlaussCPU::JMPULEREL  : KlaussCPU::JMPULE;  break;
    case ISD::SETUGT:  JmpOpc = PIC ? KlaussCPU::JMPUGTREL  : KlaussCPU::JMPUGT;  break;
    case ISD::SETUGE:  JmpOpc = PIC ? KlaussCPU::JMPUGEREL  : KlaussCPU::JMPUGE;  break;
    default:
      llvm_unreachable("KlaussCPU BR_CC: unhandled CondCode");
    }

    SDValue JmpOps[] = {Dest, SDValue(CmpNode, 0) /* chain from compare */};
    SDNode *JmpNode = CurDAG->getMachineNode(JmpOpc, DL, MVT::Other, JmpOps);
    ReplaceNode(N, JmpNode);
    return;
  }

  // ---- KlaussCPUISD::CALL → CALLREL in PIC mode ----------------------------
  // In non-PIC mode, direct calls fall through to SelectCode() which matches
  // the CALL_I tablegen patterns (tglobaladdr / texternalsym).
  // In PIC mode we intercept here and emit CALLREL (PC-relative, PCREL32 fixup).
  // Indirect calls (GPR callee → CALLR_R) always fall through to SelectCode().
  if (N->getOpcode() == KlaussCPUISD::CALL && TM.isPositionIndependent()) {
    SDValue Callee = N->getOperand(1);
    if (Callee.getOpcode() == ISD::TargetGlobalAddress ||
        Callee.getOpcode() == ISD::TargetExternalSymbol) {
      SDLoc DL(N);
      SDValue Chain = N->getOperand(0);
      SDValue LastOp = N->getOperand(N->getNumOperands() - 1);
      bool HasGlue = (LastOp.getValueType() == MVT::Glue);
      // Build operands: callee first, then any register-read operands
      // (R0/R1/SP from LowerCall's RegsToPass), then chain, then glue.
      // The register operands are SDValues of type != Other/Glue between
      // position 2 and the end of the operand list; they become implicit-use
      // entries in the MachineInstr so regalloc knows CALLREL reads them.
      SmallVector<SDValue, 8> Ops = {Callee};
      for (unsigned i = 2, e = N->getNumOperands(); i != e; ++i) {
        SDValue Op = N->getOperand(i);
        if (Op.getValueType() == MVT::Glue) break;
        Ops.push_back(Op);
      }
      Ops.push_back(Chain);
      if (HasGlue) Ops.push_back(LastOp);
      SDNode *Res = CurDAG->getMachineNode(KlaussCPU::CALLREL, DL,
                                            N->getVTList(), Ops);
      ReplaceNode(N, Res);
      return;
    }
    // Indirect call: fall through to SelectCode() → CALLR_R pattern.
  }

  // ---- KlaussCPUISD::SELECT → SELECT_PSEUDO (custom inserter) -------------
  if (N->getOpcode() == KlaussCPUISD::SELECT) {
    SDLoc DL(N);
    SDNode *Res = CurDAG->getMachineNode(
        KlaussCPU::SELECT_PSEUDO, DL, N->getValueType(0),
        {N->getOperand(0), N->getOperand(1), N->getOperand(2)});
    ReplaceNode(N, Res);
    return;
  }

  // ---- ISD::TRAP → HALT_I --------------------------------------------------
  if (N->getOpcode() == ISD::TRAP) {
    SDValue Chain = N->getOperand(0);
    SDLoc DL(N);
    SDNode *Res = CurDAG->getMachineNode(KlaussCPU::HALT_I, DL,
                                         MVT::Other, {Chain});
    ReplaceNode(N, Res);
    return;
  }

  // Delegate everything else to the tablegen-generated SelectCode().
  SelectCode(N);
}

// Factory function called from KlaussCPUPassConfig::addInstSelector().
FunctionPass *llvm::createKlaussCPUISelDag(KlaussCPUTargetMachine &TM) {
  return new KlaussCPUDAGToDAGISelLegacy(TM);
}
