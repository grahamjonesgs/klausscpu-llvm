//===-- KlaussCPUISelLowering.h - KlaussCPU DAG Lowering --------*- C++ -*-===//
//
// KlaussCPU LLVM backend — SelectionDAG lowering class declaration.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUISELLOWERING_H
#define LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUISELLOWERING_H

#include "llvm/CodeGen/TargetLowering.h"

namespace llvm {

class KlaussCPUSubtarget;

// KlaussCPU-specific SelectionDAG node types.
namespace KlaussCPUISD {
enum NodeType : unsigned {
  FIRST_NUMBER = ISD::BUILTIN_OP_END,
  RET_GLUE, // function return with optional return-value glue
  CALL,     // direct call (step 8)
  ADDR,     // materialise a global/external-symbol address into a GPR
            //   (i64) = ADDR (TargetGlobalAddress/TargetExternalSymbol)
            // Lowered to SETR by Select().  Wrapping is necessary because
            // TargetGlobalAddress CSEs, so we cannot pass it as an operand
            // to getMachineNode() and then call ReplaceNode() on it — that
            // would cause ReplaceAllUsesWith to create a self-referential
            // SETR instruction ("%0 = SETR %0").
  SELECT,   // (cond:i64, trueV:i64, falseV:i64) -> i64
            // Custom ISD node: lowered by Select() to SELECT_PSEUDO, then
            // expanded to branches by EmitInstrWithCustomInserter.
};
} // namespace KlaussCPUISD

class KlaussCPUTargetLowering : public TargetLowering {
public:
  explicit KlaussCPUTargetLowering(const TargetMachine &TM,
                                    const KlaussCPUSubtarget &STI);

  SDValue LowerOperation(SDValue Op, SelectionDAG &DAG) const override;

  const char *getTargetNodeName(unsigned Opcode) const override;

  SDValue LowerFormalArguments(SDValue Chain, CallingConv::ID CallConv,
                                bool IsVarArg,
                                const SmallVectorImpl<ISD::InputArg> &Ins,
                                const SDLoc &DL, SelectionDAG &DAG,
                                SmallVectorImpl<SDValue> &InVals) const override;

  SDValue LowerReturn(SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
                       const SmallVectorImpl<ISD::OutputArg> &Outs,
                       const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
                       SelectionDAG &DAG) const override;

  SDValue LowerCall(TargetLowering::CallLoweringInfo &CLI,
                     SmallVectorImpl<SDValue> &InVals) const override;

private:
  SDValue LowerGlobalAddress(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerExternalSymbol(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerSTACKSAVE(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerSTACKRESTORE(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerSELECT(SDValue Op, SelectionDAG &DAG) const;

public:
  MachineBasicBlock *
  EmitInstrWithCustomInserter(MachineInstr &MI,
                               MachineBasicBlock *BB) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUISELLOWERING_H
