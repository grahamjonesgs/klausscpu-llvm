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
  TAIL,     // tail call: a jump to the callee after the epilogue
  ADDR,     // materialise a global address via SETRU (non-PIC)
            //   (i64) = ADDR (TargetGlobalAddress/TargetExternalSymbol/...)
            // Selected to SETRU by a pattern.  The wrapper is necessary because
            // TargetGlobalAddress CSEs: selecting it in place would replace it
            // in the SETRU's own operand ("%0 = SETRU %0").
  LEAPC,    // materialise a global address via LEAPC (PIC mode)
            //   (i64) = LEAPC (TargetGlobalAddress/TargetExternalSymbol/...)
            // Selected to LEAPC by a pattern.  Same wrapper as ADDR.
  SELECT_CC, // (lhs:i64, rhs:i64, cc:CondCode, trueV:i64, falseV:i64) -> i64
            // Selected to SELECT_CC_RR/RI, then expanded to compare +
            // conditional jump + PHI by EmitInstrWithCustomInserter.
  ROLW,     // (x:i64, amt:i64) -> i64: low 32 bits = rotl32(x[31:0], amt),
  RORW,     // rotr32; the upper 32 bits are the same 32 bits (not an
            // extension).  From ReplaceNodeResults for an i32 ROTL/ROTR;
            // selected to a 64-bit rotate of x copied into both halves.
};
} // namespace KlaussCPUISD

class KlaussCPUTargetLowering : public TargetLowering {
public:
  explicit KlaussCPUTargetLowering(const TargetMachine &TM,
                                    const KlaussCPUSubtarget &STI);

  SDValue LowerOperation(SDValue Op, SelectionDAG &DAG) const override;

  const char *getTargetNodeName(unsigned Opcode) const override;

  // CLZ/CTZ are single-cycle and return 64 for input 0 (hardware-confirmed),
  // so CodeGenPrepare must not despeculate zero-defined ctlz/cttz into a
  // zero-check branch around the count.
  bool isCheapToSpeculateCtlz(Type *Ty) const override { return true; }
  bool isCheapToSpeculateCttz(Type *Ty) const override { return true; }

  // Never narrow a wide load into a smaller one.  KlaussCPU has a single
  // 64-bit register class, so narrowing saves no register and no
  // instruction.  History: this was added when the combine turned
  // LowerVAARG's full-slot load + truncate into a register-addressed
  // MEMGET32 that read the wrong va_arg slot (mixed-width printf "%u"
  // garbled, 2026-06).  That was a fault of the pre-pipeline core; on the
  // pipeline core MEMGET32 is correct at any address (unaligned 32-bit loads
  // rely on it) and clang's va_arg loads the slot at the argument's width.
  // Whether narrowing would now pay off has not been measured.
  bool shouldReduceLoadWidth(SDNode *Load, ISD::LoadExtType ExtTy, EVT NewVT,
                             std::optional<unsigned> ByteOffset) const override {
    return false;
  }

  // Every sub-i64 load (LDIDX8/16/32, MEMGET8/16/32) zero-extends into the
  // 64-bit register, so zero-extending a loaded value costs nothing.  This
  // lets the combiner turn anyext loads that also feed a zext into ZEXTLOADs
  // instead of emitting a separate ZEXTW/ZEXTB/ZEXTH.
  bool isZExtFree(SDValue Val, EVT VT2) const override;

  // What loop strength reduction and CodeGenPrepare may fold into an access
  // or an add/compare: exactly what isel selects (see the .cpp).
  bool isLegalAddressingMode(const DataLayout &DL, const AddrMode &AM, Type *Ty,
                             unsigned AS,
                             Instruction *I = nullptr) const override;
  bool isLegalAddImmediate(int64_t Imm) const override;
  bool isLegalICmpImmediate(int64_t Imm) const override;

  // Multiply by a constant: MULV takes a simm32, holds EX for the multiplier
  // latency and has no short form; a shift and an add/sub may be cheaper.
  bool decomposeMulByConstant(LLVMContext &Context, EVT VT,
                              SDValue C) const override;
  bool isMulAddWithConstProfitable(SDValue AddNode,
                                   SDValue ConstNode) const override;

  // Under-aligned 32-bit loads (MEMGET32) and inline memcpy / memset /
  // memmove (see the .cpp): the access width both sides' alignment allows,
  // constant sources as immediates, and 8-byte alignment for the stack
  // objects and globals they copy.
  EVT getOptimalMemOpType(LLVMContext &Context, const MemOp &Op,
                          const AttributeList &FuncAttributes) const override;
  bool findOptimalMemOpLowering(LLVMContext &Context, std::vector<EVT> &MemOps,
                                unsigned Limit, const MemOp &Op, unsigned DstAS,
                                unsigned SrcAS,
                                const AttributeList &FuncAttributes,
                                EVT *LargestVT = nullptr) const override;
  bool allowsMisalignedMemoryAccesses(EVT VT, unsigned AddrSpace,
                                      Align Alignment,
                                      MachineMemOperand::Flags Flags,
                                      unsigned *Fast) const override;
  bool shouldConvertConstantLoadToIntImm(const APInt &Imm,
                                         Type *Ty) const override;

  // The two 32-bit halves of an under-aligned i64 load (zero-extending
  // loads of [p] and [p + 4], marked KlaussCPU::MOSplitHalf) and their
  // joined chain.  joinHalves gives the i64 value: (Hi << 32) | Lo.
  struct SplitLoad64 {
    SDValue Lo, Hi, Chain;
  };
  static SplitLoad64 splitUnalignedLoad64(LoadSDNode *LD, SelectionDAG &DAG);
  static SDValue joinHalves(const SplitLoad64 &S, const SDLoc &DL,
                            SelectionDAG &DAG);
  bool shouldAlignPointerArgs(CallInst *CI, unsigned &MinSize,
                              Align &PrefAlign) const override;

  SDValue PerformDAGCombine(SDNode *N, DAGCombinerInfo &DCI) const override;

  // Library calls follow the C ABI (ABI.md): an i32 argument or result is
  // sign-extended to 64 bits whether it is signed or not, as on RV64, and a
  // soft-float f32 is passed in the low half with no extension.  A softened
  // f16 is still zero-extended: clang has no _Float16 for this target, so
  // compiler-rt's half-precision functions (__extendhfsf2, __truncsfhf2, ...)
  // take and return a uint16_t, which the C ABI zero-extends.
  bool shouldSignExtendTypeInLibCall(Type *Ty, bool IsSigned) const override {
    return Ty->isIntegerTy(32) || IsSigned;
  }
  bool shouldExtendTypeInLibCall(EVT Type) const override {
    return Type != MVT::f32;
  }

  // Forced atomics: which atomicrmw ops AtomicExpand turns into cmpxchg loops.
  AtomicExpansionKind
  shouldExpandAtomicRMWInIR(const AtomicRMWInst *AI) const override;

  void ReplaceNodeResults(SDNode *N, SmallVectorImpl<SDValue> &Results,
                          SelectionDAG &DAG) const override;

  SDValue LowerFormalArguments(SDValue Chain, CallingConv::ID CallConv,
                                bool IsVarArg,
                                const SmallVectorImpl<ISD::InputArg> &Ins,
                                const SDLoc &DL, SelectionDAG &DAG,
                                SmallVectorImpl<SDValue> &InVals) const override;

  bool CanLowerReturn(CallingConv::ID CallConv, MachineFunction &MF,
                      bool IsVarArg,
                      const SmallVectorImpl<ISD::OutputArg> &Outs,
                      LLVMContext &Context, const Type *RetTy) const override;

  SDValue LowerReturn(SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
                       const SmallVectorImpl<ISD::OutputArg> &Outs,
                       const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
                       SelectionDAG &DAG) const override;

  SDValue LowerCall(TargetLowering::CallLoweringInfo &CLI,
                     SmallVectorImpl<SDValue> &InVals) const override;

  // Whether CodeGenPrepare may duplicate a return into its predecessors so
  // that a call there becomes a tail call (-klausscpu-tail-call-dup-ret).
  bool mayBeEmittedAsTailCall(const CallInst *CI) const override;

private:
  const KlaussCPUSubtarget &Subtarget;

  bool isEligibleForTailCallOptimization(
      const CCState &CCInfo, const TargetLowering::CallLoweringInfo &CLI,
      const MachineFunction &MF) const;

  SDValue LowerGlobalAddress(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerExternalSymbol(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerBlockAddress(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerJumpTable(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerBR_JT(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerLOAD(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerSTACKSAVE(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerSTACKRESTORE(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerDYNAMIC_STACKALLOC(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerSELECT(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerSELECT_CC(SDValue Op, SelectionDAG &DAG) const;
  SDValue buildSelectCC(const SDLoc &DL, SDValue LHS, SDValue RHS,
                        ISD::CondCode CC, SDValue TrueV, SDValue FalseV,
                        SelectionDAG &DAG) const;
  SDValue LowerVASTART(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerVAARG(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerShiftLeftParts(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerShiftRightParts(SDValue Op, SelectionDAG &DAG, bool IsSRA) const;
  SDValue LowerATOMIC_FENCE(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerFRAMEADDR(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerRETURNADDR(SDValue Op, SelectionDAG &DAG) const;

public:
  // Inline assembly constraint support.
  std::pair<unsigned, const TargetRegisterClass *>
  getRegForInlineAsmConstraint(const TargetRegisterInfo *TRI,
                                StringRef Constraint, MVT VT) const override;

  unsigned getJumpTableEncoding() const override;

  // Jump tables need -klausscpu-jump-table-density (40%) cases among their
  // entries, at every optimisation level.
  bool isSuitableForJumpTable(const SwitchInst *SI, uint64_t NumCases,
                              uint64_t Range, ProfileSummaryInfo *PSI,
                              BlockFrequencyInfo *BFI) const override;

  // EK_Custom32 entries are ABSOLUTE block addresses, in PIC mode too.  The
  // default (== isPositionIndependent()) would make BR_JT add the table base
  // to each entry under -fPIC, jumping to table+target.
  bool isJumpTableRelative() const override { return false; }

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
