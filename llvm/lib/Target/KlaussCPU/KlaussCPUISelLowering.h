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
  CALL,     // direct/indirect call
  ADDR,     // materialise a global address via SETR (non-PIC)
            //   (i64) = ADDR (TargetGlobalAddress/TargetExternalSymbol)
            // Lowered to SETR by Select().  ADDR wrapper is necessary because
            // TargetGlobalAddress CSEs — passing it directly to getMachineNode
            // and then calling ReplaceNode on it would cause a self-referential
            // SETR instruction ("%0 = SETR %0").
  LEAPC,    // materialise a global address via LEAPC (PIC mode)
            //   (i64) = LEAPC (TargetGlobalAddress/TargetExternalSymbol)
            // Lowered to LEAPC by Select().  Same CSE-safe wrapper as ADDR.
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
  SDValue LowerBlockAddress(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerJumpTable(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerBR_JT(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerSTACKSAVE(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerSTACKRESTORE(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerDYNAMIC_STACKALLOC(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerSELECT(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerVASTART(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerShiftLeftParts(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerShiftRightParts(SDValue Op, SelectionDAG &DAG, bool IsSRA) const;

public:
  // Inline assembly constraint support.
  std::pair<unsigned, const TargetRegisterClass *>
  getRegForInlineAsmConstraint(const TargetRegisterInfo *TRI,
                                StringRef Constraint, MVT VT) const override;

  unsigned getJumpTableEncoding() const override;

  // EK_Custom32: emit each entry as a 4-byte absolute target address.
  // The assembler emits R_KLAUSSCPU_ABS32 for each entry; the linker fills
  // in the correct address regardless of which ELF section the table lands in.
  const MCExpr *LowerCustomJumpTableEntry(const MachineJumpTableInfo *,
                                           const MachineBasicBlock *MBB,
                                           unsigned, MCContext &Ctx) const override;


  MachineBasicBlock *
  EmitInstrWithCustomInserter(MachineInstr &MI,
                               MachineBasicBlock *BB) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_KLAUSSCPUISELLOWERING_H
