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
#include "llvm/Support/Debug.h"

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

  // Delegate everything else to the tablegen-generated SelectCode().
  SelectCode(N);
}

// Factory function called from KlaussCPUPassConfig::addInstSelector().
FunctionPass *llvm::createKlaussCPUISelDag(KlaussCPUTargetMachine &TM) {
  return new KlaussCPUDAGToDAGISelLegacy(TM);
}
