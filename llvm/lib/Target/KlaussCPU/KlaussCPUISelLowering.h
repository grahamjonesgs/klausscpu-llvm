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
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUISELLOWERING_H
