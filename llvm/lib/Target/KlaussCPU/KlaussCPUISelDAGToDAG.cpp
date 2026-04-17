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

  // ---- ISD::FrameIndex → TargetFrameIndex --------------------------------
  // KlaussCPU has no i32 register class, so pointers are promoted from i32 to
  // i64 during type legalization.  We produce a TargetFrameIndex typed as i64
  // so it can serve as a GPR-class operand in LDIDX64 / STIDX64.
  // eliminateFrameIndex (KlaussCPURegisterInfo) later rewrites the operand to
  // R15 + frame_slot_offset.
  if (N->getOpcode() == ISD::FrameIndex) {
    int FI = cast<FrameIndexSDNode>(N)->getIndex();
    SDValue TFI = CurDAG->getTargetFrameIndex(FI, MVT::i64);
    ReplaceNode(N, TFI.getNode());
    return;
  }

  // TargetFrameIndex is a legal operand placeholder; no instruction needed.
  if (N->getOpcode() == ISD::TargetFrameIndex)
    return;

  // ---- KlaussCPUISD::ADDR → SETR -------------------------------------------
  // LowerGlobalAddress / LowerExternalSymbol wrap the TargetGlobalAddress in
  // a KlaussCPUISD::ADDR node (see KlaussCPUISelLowering.h for the reason).
  //
  // Here we peel off the wrapper and emit SETR with the enclosed symbol.
  //
  // Why not handle bare TargetGlobalAddress here instead?  Two reasons:
  //   1. SelectionDAG CSE-deduplicates TargetGlobalAddress nodes, so
  //      creating a "fresh" copy returns the original node.  Passing it as
  //      getMachineNode's operand and then calling ReplaceNode on it causes
  //      ReplaceAllUsesWith to rewrite SETR's own operand reference → self-
  //      referential machine instruction ("%0 = SETR %0").
  //   2. Call-site callee addresses bypass LowerOperation (LowerCall calls
  //      DAG.getTargetGlobalAddress directly) and are handled by the
  //      CALL_I tablegen pattern — they must NOT be converted to SETR.
  if (N->getOpcode() == KlaussCPUISD::ADDR) {
    // N = KlaussCPUISD::ADDR(TGA_or_TES)
    // Operand 0 is the TargetGlobalAddress or TargetExternalSymbol leaf.
    SDValue Sym = N->getOperand(0);
    SDLoc DL(N);
    // getMachineNode with Sym (TGA/TES) as operand → MO_GlobalAddress in the
    // resulting MachineInstr.  ReplaceNode replaces ADDR (N), not Sym, so
    // SETR's reference to Sym is not rewritten → no self-reference.
    SDNode *Res =
        CurDAG->getMachineNode(KlaussCPU::SETR, DL, MVT::i64, Sym);
    ReplaceNode(N, Res);
    return;
  }

  // ---- Frame-slot LOAD: (load (TargetFrameIndex)) → LDIDX64 / LDIDX32 ---
  // The 0 offset is a placeholder; eliminateFrameIndex will fill in the real
  // frame offset (R15 + slot_offset).  Machine node order: [base, offset, chain].
  //
  // i8/i16 frame-slot loads require address materialisation via a scratch
  // register (register scavenging) which is not yet implemented.  Those cases
  // fall through to SelectCode and will produce a "Cannot select" error.
  // Use -O1 or higher to eliminate frame-local i8/i16 allocas via mem2reg.
  if (N->getOpcode() == ISD::LOAD) {
    auto *LN = cast<LoadSDNode>(N);
    SDValue Ptr = LN->getBasePtr();
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
    }
  }

  // ---- Frame-slot STORE: (store val, (TargetFrameIndex)) → STIDX64 / STIDX32 ---
  // Machine node order: [data, base, offset, chain].
  if (N->getOpcode() == ISD::STORE) {
    auto *SN = cast<StoreSDNode>(N);
    SDValue Ptr = SN->getBasePtr();
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
  // SETR covers [-2^31, 2^31-1].  For larger values, emit the 3-instruction
  // sequence:  SETR rd, hi32  →  SHLV rd, 32  →  ORV rd, lo32
  // giving  rd = {hi32, lo32}  (full 64-bit value).
  //
  // NOTE: SETR64 (hardware instruction) has a known hi32 fetch bug
  // (r_PC[2] condition inverted) and must NOT be used.
  if (N->getOpcode() == ISD::Constant) {
    auto *C = cast<ConstantSDNode>(N);
    int64_t Val = C->getSExtValue();
    if (!isInt<32>(Val)) {
      SDLoc DL(N);
      uint32_t Lo = static_cast<uint32_t>(static_cast<uint64_t>(Val));
      int32_t  Hi = static_cast<int32_t>(static_cast<uint64_t>(Val) >> 32);

      // SETR rd, Hi   (sign-extends Hi to 64 bits)
      SDValue HiImm = CurDAG->getTargetConstant(Hi, DL, MVT::i64);
      SDNode *SetR  = CurDAG->getMachineNode(KlaussCPU::SETR, DL, MVT::i64, HiImm);

      // SHLV rd, 32   (rd = rd << 32 → {Hi, 0})
      SDValue ShAmt = CurDAG->getTargetConstant(32, DL, MVT::i64);
      SDNode *Shlv  = CurDAG->getMachineNode(KlaussCPU::SHLV, DL, MVT::i64,
                                              {SDValue(SetR, 0), ShAmt});

      // ORV rd, Lo    (rd = rd | zero_ext(Lo) → {Hi, Lo})
      SDValue LoImm = CurDAG->getTargetConstant(static_cast<int32_t>(Lo),
                                                 DL, MVT::i64);
      SDNode *Orv   = CurDAG->getMachineNode(KlaussCPU::ORV, DL, MVT::i64,
                                              {SDValue(Shlv, 0), LoImm});

      ReplaceNode(N, Orv);
      return;
    }
    // simm32 constants: fall through to SelectCode (matched by SETR pattern).
  }

  // ---- ISD::BR → JMP (unconditional branch) --------------------------------
  // ISD::BR: (chain, dest_bb)
  if (N->getOpcode() == ISD::BR) {
    SDValue Chain = N->getOperand(0);
    SDValue Dest  = N->getOperand(1);
    SDLoc DL(N);
    SDValue Ops[] = {Dest, Chain};
    SDNode *Res = CurDAG->getMachineNode(KlaussCPU::JMP, DL, MVT::Other, Ops);
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
    unsigned JmpOpc;
    switch (CC) {
    case ISD::SETEQ:   JmpOpc = KlaussCPU::JMPE;    break;
    case ISD::SETNE:   JmpOpc = KlaussCPU::JMPNE;   break;
    case ISD::SETLT:   JmpOpc = KlaussCPU::JMPLT;   break;
    case ISD::SETLE:   JmpOpc = KlaussCPU::JMPLE;   break;
    case ISD::SETGT:   JmpOpc = KlaussCPU::JMPGT;   break;
    case ISD::SETGE:   JmpOpc = KlaussCPU::JMPGE;   break;
    case ISD::SETULT:  JmpOpc = KlaussCPU::JMPULT;  break;
    case ISD::SETULE:  JmpOpc = KlaussCPU::JMPULE;  break;
    case ISD::SETUGT:  JmpOpc = KlaussCPU::JMPUGT;  break;
    case ISD::SETUGE:  JmpOpc = KlaussCPU::JMPUGE;  break;
    default:
      llvm_unreachable("KlaussCPU BR_CC: unhandled CondCode");
    }

    SDValue JmpOps[] = {Dest, SDValue(CmpNode, 0) /* chain from compare */};
    SDNode *JmpNode = CurDAG->getMachineNode(JmpOpc, DL, MVT::Other, JmpOps);
    ReplaceNode(N, JmpNode);
    return;
  }

  // Delegate everything else to the tablegen-generated SelectCode().
  SelectCode(N);
}

// Factory function called from KlaussCPUPassConfig::addInstSelector().
FunctionPass *llvm::createKlaussCPUISelDag(KlaussCPUTargetMachine &TM) {
  return new KlaussCPUDAGToDAGISelLegacy(TM);
}
