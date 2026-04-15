//===-- KlaussCPUISelLowering.cpp - KlaussCPU DAG Lowering ----------------===//
//
// KlaussCPU LLVM backend — SelectionDAG lowering implementation.
//
// Key architecture constraints:
//   - No hardware FP               → soft-float
//   - No CMOV                      → SELECT must be expanded
//   - No atomics                   → setMaxAtomicSizeInBitsSupported(0)
//   - No sign-extending loads      → SEXTLOAD i8/i16/i32 → Expand
//   - Hardware integer ops         → div, mul-high, min/max, setcc are Legal
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUISelLowering.h"
#include "KlaussCPURegisterInfo.h"
#include "KlaussCPUSubtarget.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

KlaussCPUTargetLowering::KlaussCPUTargetLowering(const TargetMachine &TM,
                                                   const KlaussCPUSubtarget &STI)
    : TargetLowering(TM, STI) {

  // ---- Register class setup ----
  addRegisterClass(MVT::i64, &KlaussCPU::GPRRegClass);

  // Compute derived properties from the register classes.
  computeRegisterProperties(STI.getRegisterInfo());

  // The hardware stack pointer is a separate 32-bit resource; tell LLVM
  // which register to use for stack save/restore.
  setStackPointerRegisterToSaveRestore(KlaussCPU::SP);

  // ---- Soft-float ----
  // KlaussCPU has no FP hardware; all floating-point must be lowered to libcalls.
  setOperationAction(ISD::FADD,  MVT::f32, Expand);
  setOperationAction(ISD::FSUB,  MVT::f32, Expand);
  setOperationAction(ISD::FMUL,  MVT::f32, Expand);
  setOperationAction(ISD::FDIV,  MVT::f32, Expand);
  setOperationAction(ISD::FADD,  MVT::f64, Expand);
  setOperationAction(ISD::FSUB,  MVT::f64, Expand);
  setOperationAction(ISD::FMUL,  MVT::f64, Expand);
  setOperationAction(ISD::FDIV,  MVT::f64, Expand);

  // ---- No CMOV → expand SELECT ----
  for (MVT VT : MVT::integer_valuetypes()) {
    setOperationAction(ISD::SELECT,    VT, Expand);
    setOperationAction(ISD::SELECT_CC, VT, Expand);
  }

  // ---- No sign-extending loads ----
  // The hardware only does zero-extending loads; SEXT loads must be expanded.
  for (MVT VT : {MVT::i8, MVT::i16, MVT::i32}) {
    setLoadExtAction(ISD::SEXTLOAD, MVT::i64, VT, Expand);
    setLoadExtAction(ISD::SEXTLOAD, MVT::i32, VT, Expand);
  }

  // ---- No atomics ----
  setMaxAtomicSizeInBitsSupported(0);

  // ---- Stack operations ----
  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i32, Expand);
  setOperationAction(ISD::STACKSAVE,          MVT::Other, Expand);
  setOperationAction(ISD::STACKRESTORE,       MVT::Other, Expand);

  // ---- Branch/control ----
  setOperationAction(ISD::BR_JT,  MVT::Other, Expand);
  setOperationAction(ISD::BRCOND, MVT::Other, Expand); // use BR_CC

  // ---- Integer ops that the hardware supports natively ----
  // These are Legal by default once the register class is registered,
  // but make them explicit for clarity:
  //   ADDR/SUBR/MULR → Legal (i64)
  //   DIVR/DIVUR/MODR/MODUR → Legal (i64)
  //   MULHR/MULHUR → Legal as MULHS/MULHU (i64)
  //   SHLR/SHRR/SARR → Legal (i64)
  //   CMPEQR/CMPNER/… → Legal as SETCC (i64)
  //   MINR/MAXR/MINUR/MAXUR → Legal (i64)
  setOperationAction(ISD::MULHS, MVT::i64, Legal);
  setOperationAction(ISD::MULHU, MVT::i64, Legal);
  setOperationAction(ISD::SDIV,  MVT::i64, Legal);
  setOperationAction(ISD::UDIV,  MVT::i64, Legal);
  setOperationAction(ISD::SREM,  MVT::i64, Legal);
  setOperationAction(ISD::UREM,  MVT::i64, Legal);
  setOperationAction(ISD::SMIN,  MVT::i64, Legal);
  setOperationAction(ISD::SMAX,  MVT::i64, Legal);
  setOperationAction(ISD::UMIN,  MVT::i64, Legal);
  setOperationAction(ISD::UMAX,  MVT::i64, Legal);
  setOperationAction(ISD::SETCC, MVT::i64, Legal);

  // ---- Varargs ----
  setOperationAction(ISD::VASTART, MVT::Other, Expand);
  setOperationAction(ISD::VAARG,   MVT::Other, Expand);
  setOperationAction(ISD::VACOPY,  MVT::Other, Expand);
  setOperationAction(ISD::VAEND,   MVT::Other, Expand);

  // ---- Misc ----
  // Only 32-bit and 64-bit native integer widths.
  setMinFunctionAlignment(Align(4));
  setPrefFunctionAlignment(Align(4));
}

SDValue KlaussCPUTargetLowering::LowerOperation(SDValue Op,
                                                 SelectionDAG &DAG) const {
  llvm_unreachable("KlaussCPU: unimplemented LowerOperation opcode");
}

const char *
KlaussCPUTargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch (Opcode) {
  case KlaussCPUISD::RET_GLUE: return "KlaussCPUISD::RET_GLUE";
  case KlaussCPUISD::CALL:     return "KlaussCPUISD::CALL";
  default: return nullptr;
  }
}

//===----------------------------------------------------------------------===//
// Calling convention — hardcoded until KlaussCPUCallingConv.td (step 8)
//
//   Args 0–3  : R0–R3  (i64, caller-saved)
//   Args 4+   : stack  (8 bytes each, offset 32 + n*8 from caller SP)
//   Return    : R12    (M, caller-saved)
//===----------------------------------------------------------------------===//

SDValue KlaussCPUTargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID /*CallConv*/, bool /*IsVarArg*/,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  MachineFunction &MF = DAG.getMachineFunction();
  MachineRegisterInfo &RegInfo = MF.getRegInfo();

  static const MCPhysReg ArgRegs[] = {
      KlaussCPU::R0, KlaussCPU::R1, KlaussCPU::R2, KlaussCPU::R3};

  unsigned RegIdx = 0;
  int64_t StackOffset = 32; // first stack arg is at [CallerSP + 32]

  for (unsigned i = 0, e = Ins.size(); i != e; ++i) {
    if (RegIdx < 4) {
      Register VReg = RegInfo.createVirtualRegister(&KlaussCPU::GPRRegClass);
      RegInfo.addLiveIn(ArgRegs[RegIdx++], VReg);
      InVals.push_back(DAG.getCopyFromReg(Chain, DL, VReg, MVT::i64));
    } else {
      // Stack argument — create a fixed stack object and load from it.
      MachineFrameInfo &MFI = MF.getFrameInfo();
      int FI = MFI.CreateFixedObject(8, StackOffset, /*isImmutable=*/true);
      SDValue FIN = DAG.getFrameIndex(FI, getPointerTy(DAG.getDataLayout()));
      InVals.push_back(DAG.getLoad(MVT::i64, DL, Chain, FIN,
                                    MachinePointerInfo::getFixedStack(MF, FI)));
      StackOffset += 8;
      ++RegIdx;
    }
  }

  return Chain;
}

SDValue KlaussCPUTargetLowering::LowerReturn(
    SDValue Chain, CallingConv::ID /*CallConv*/, bool /*IsVarArg*/,
    const SmallVectorImpl<ISD::OutputArg> &Outs,
    const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
    SelectionDAG &DAG) const {
  SmallVector<SDValue, 4> RetOps(1, Chain);
  SDValue Glue;

  // KlaussCPU ABI: single i64 return value goes in R12 (M register).
  if (!Outs.empty()) {
    assert(Outs.size() == 1 && "KlaussCPU: only one i64 return value supported");
    Chain = DAG.getCopyToReg(Chain, DL, KlaussCPU::R12, OutVals[0], Glue);
    Glue = Chain.getValue(1);
    RetOps.push_back(DAG.getRegister(KlaussCPU::R12, MVT::i64));
  }

  RetOps[0] = Chain;
  if (Glue.getNode())
    RetOps.push_back(Glue);

  return DAG.getNode(KlaussCPUISD::RET_GLUE, DL, MVT::Other, RetOps);
}

SDValue KlaussCPUTargetLowering::LowerCall(
    TargetLowering::CallLoweringInfo & /*CLI*/,
    SmallVectorImpl<SDValue> & /*InVals*/) const {
  report_fatal_error("KlaussCPU: LowerCall not yet implemented (step 8)");
}
