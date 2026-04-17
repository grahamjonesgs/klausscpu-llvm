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

// Generated calling convention functions.  Must be included inside namespace
// llvm because the generated code uses unqualified names (MVT, CCState, etc.).
namespace llvm {
#include "KlaussCPUGenCallingConv.inc"
} // namespace llvm

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

  // ---- Sub-word loads / stores ----
  // Hardware has MEMGET8/16/32 (zero-extending, register-addressed) and
  // LDIDX32/STIDX32 (base+offset, 32-bit).  No hardware sign-extending loads.
  //
  // SEXTLOAD expands → ZEXTLOAD + SIGN_EXTEND_INREG.
  // SIGN_EXTEND_INREG i8/i16 → Legal (SEXTB/SEXTH hardware instructions).
  // SIGN_EXTEND_INREG i32 → Expand (shift pair: SHLV 32 + SHRAV 32).
  for (MVT VT : {MVT::i8, MVT::i16, MVT::i32}) {
    setLoadExtAction(ISD::ZEXTLOAD, MVT::i64, VT, Legal);
    setLoadExtAction(ISD::EXTLOAD,  MVT::i64, VT, Legal);
    setLoadExtAction(ISD::SEXTLOAD, MVT::i64, VT, Expand);
    setLoadExtAction(ISD::SEXTLOAD, MVT::i32, VT, Expand);
  }
  setTruncStoreAction(MVT::i64, MVT::i8,  Legal);
  setTruncStoreAction(MVT::i64, MVT::i16, Legal);
  setTruncStoreAction(MVT::i64, MVT::i32, Legal);
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i8,  Legal);
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i16, Legal);
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i32, Expand);

  // ---- No atomics ----
  setMaxAtomicSizeInBitsSupported(0);

  // ---- Stack operations ----
  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i64, Expand);
  setOperationAction(ISD::STACKSAVE,          MVT::Other, Expand);
  setOperationAction(ISD::STACKRESTORE,       MVT::Other, Expand);

  // ---- Global addresses / symbols ----
  // All symbols are 32-bit addresses (128 MiB RAM); lowered to TargetGlobalAddress
  // which the isel pattern maps to SETR (sign-extends; bit 31 is always 0 for
  // valid addresses, so sign-ext == zero-ext).
  setOperationAction(ISD::GlobalAddress,  MVT::i64, Custom);
  setOperationAction(ISD::ExternalSymbol, MVT::i64, Custom);

  // ---- Branch/control ----
  setOperationAction(ISD::BR_JT,  MVT::Other, Expand);
  setOperationAction(ISD::BRCOND, MVT::Other, Expand); // expands to BR_CC
  // BR_CC with i64 operands is Legal — handled by KlaussCPUDAGToDAGISel::Select()
  // which emits CMPRR_I/CMPRV_I + the appropriate conditional JMP instruction.
  setOperationAction(ISD::BR_CC,  MVT::i64,   Legal);

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
  switch (Op.getOpcode()) {
  case ISD::GlobalAddress:  return LowerGlobalAddress(Op, DAG);
  case ISD::ExternalSymbol: return LowerExternalSymbol(Op, DAG);
  default:
    llvm_unreachable("KlaussCPU: unimplemented LowerOperation opcode");
  }
}

SDValue KlaussCPUTargetLowering::LowerGlobalAddress(SDValue Op,
                                                      SelectionDAG &DAG) const {
  auto *N = cast<GlobalAddressSDNode>(Op);
  // Wrap the TargetGlobalAddress in a KlaussCPUISD::ADDR node so that
  // KlaussCPUDAGToDAGISel::Select() can safely emit SETR without hitting
  // the CSE-induced self-reference issue (see KlaussCPUISelLowering.h).
  SDValue TGA = DAG.getTargetGlobalAddress(N->getGlobal(), SDLoc(N), MVT::i64,
                                            N->getOffset());
  return DAG.getNode(KlaussCPUISD::ADDR, SDLoc(N), MVT::i64, TGA);
}

SDValue KlaussCPUTargetLowering::LowerExternalSymbol(SDValue Op,
                                                       SelectionDAG &DAG) const {
  auto *N = cast<ExternalSymbolSDNode>(Op);
  SDValue TES = DAG.getTargetExternalSymbol(N->getSymbol(), MVT::i64);
  return DAG.getNode(KlaussCPUISD::ADDR, SDLoc(N), MVT::i64, TES);
}

const char *
KlaussCPUTargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch (Opcode) {
  case KlaussCPUISD::RET_GLUE: return "KlaussCPUISD::RET_GLUE";
  case KlaussCPUISD::CALL:     return "KlaussCPUISD::CALL";
  case KlaussCPUISD::ADDR:     return "KlaussCPUISD::ADDR";
  default: return nullptr;
  }
}

//===----------------------------------------------------------------------===//
// Formal arguments — callee side
//
// Stack layout (from callee's incoming SP):
//   [SP+0..SP+7]   return address (written by CALL, SP unchanged)
//   [SP+8..SP+31]  reserved
//   [SP+32..]      stack arguments (8 bytes each)
//
// We pre-reserve 32 bytes in CCState so stack args land at offset ≥ 32.
//===----------------------------------------------------------------------===//

SDValue KlaussCPUTargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  MachineFunction &MF = DAG.getMachineFunction();
  MachineRegisterInfo &RegInfo = MF.getRegInfo();

  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, ArgLocs, *DAG.getContext());
  // Reserve [SP+0..SP+31]: return address + ABI reserved area.
  CCInfo.AllocateStack(32, Align(8));
  CCInfo.AnalyzeFormalArguments(Ins, CC_KlaussCPU);

  for (const CCValAssign &VA : ArgLocs) {
    if (VA.isRegLoc()) {
      Register VReg = RegInfo.createVirtualRegister(&KlaussCPU::GPRRegClass);
      RegInfo.addLiveIn(VA.getLocReg(), VReg);
      InVals.push_back(DAG.getCopyFromReg(Chain, DL, VReg, MVT::i64));
    } else {
      assert(VA.isMemLoc() && "Unexpected CCValAssign loc type");
      // Offset is relative to the incoming SP (already ≥ 32 due to pre-reserve).
      int FI = MF.getFrameInfo().CreateFixedObject(8, VA.getLocMemOffset(),
                                                    /*isImmutable=*/true);
      SDValue FIN = DAG.getFrameIndex(FI, getPointerTy(DAG.getDataLayout()));
      InVals.push_back(DAG.getLoad(MVT::i64, DL, Chain, FIN,
                                    MachinePointerInfo::getFixedStack(MF, FI)));
    }
  }

  return Chain;
}

//===----------------------------------------------------------------------===//
// Return — copy result to R12 and emit RET_GLUE
//===----------------------------------------------------------------------===//

SDValue KlaussCPUTargetLowering::LowerReturn(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs,
    const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
    SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();

  SmallVector<CCValAssign, 4> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, RVLocs, *DAG.getContext());
  CCInfo.AnalyzeReturn(Outs, RetCC_KlaussCPU);

  SmallVector<SDValue, 4> RetOps(1, Chain);
  SDValue Glue;

  for (const CCValAssign &VA : RVLocs) {
    assert(VA.isRegLoc() && "KlaussCPU: only register returns supported");
    Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), OutVals[VA.getValNo()],
                              Glue);
    Glue = Chain.getValue(1);
    RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
  }

  RetOps[0] = Chain;
  if (Glue.getNode())
    RetOps.push_back(Glue);

  return DAG.getNode(KlaussCPUISD::RET_GLUE, DL, MVT::Other, RetOps);
}

//===----------------------------------------------------------------------===//
// Call — outgoing side
//
// CALL instruction stores return address at [SP+0] without changing SP.
// Callee sees:  [SP+0..7]=ret_addr, [SP+8..31]=reserved, [SP+32..]=stack args.
// Since SP is unchanged across the call boundary, the caller writes stack args
// to the same offsets.  We pre-reserve 32 bytes in CCState (matching the callee
// side) so the first stack arg is assigned offset 32.
//===----------------------------------------------------------------------===//

SDValue KlaussCPUTargetLowering::LowerCall(TargetLowering::CallLoweringInfo &CLI,
                                            SmallVectorImpl<SDValue> &InVals) const {
  SelectionDAG &DAG          = CLI.DAG;
  SDLoc        &DL           = CLI.DL;
  SDValue       Chain        = CLI.Chain;
  SDValue       Callee       = CLI.Callee;
  CallingConv::ID CallConv   = CLI.CallConv;
  bool          IsVarArg     = CLI.IsVarArg;
  MachineFunction &MF        = DAG.getMachineFunction();
  // Pointers are now 64-bit (DataLayout p:64:64) so GPRs can hold them natively.
  // SP is still a 32-bit hardware register; copy it as i32 and zero-extend
  // to i64 for address arithmetic.
  MVT PtrVT  = getPointerTy(DAG.getDataLayout()); // MVT::i64
  MVT CallVT = MVT::i64;                          // GPR-width callee operand

  // ---- Analyze outgoing arguments ----------------------------------------
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, ArgLocs, *DAG.getContext());
  // Pre-reserve [SP+0..SP+31] for return address + ABI reserved area.
  CCInfo.AllocateStack(32, Align(8));
  CCInfo.AnalyzeCallOperands(CLI.Outs, CC_KlaussCPU);

  unsigned StackSize = CCInfo.getStackSize(); // ≥ 32

  Chain = DAG.getCALLSEQ_START(Chain, StackSize, 0, DL);

  // ---- Collect register and memory args ----------------------------------
  SmallVector<std::pair<Register, SDValue>, 8> RegsToPass;
  SmallVector<SDValue, 4> MemOpChains;

  // SP is a 32-bit hardware register.  Copy it as i32, then zero-extend to
  // i64 so address arithmetic uses the same type as pointer operands (i64).
  SDValue SP32 = DAG.getCopyFromReg(Chain, DL, KlaussCPU::SP, MVT::i32);
  SDValue SP   = DAG.getNode(ISD::ZERO_EXTEND, DL, PtrVT, SP32);

  for (unsigned I = 0, E = ArgLocs.size(); I != E; ++I) {
    const CCValAssign &VA = ArgLocs[I];
    SDValue Arg = CLI.OutVals[I];

    if (VA.isRegLoc()) {
      RegsToPass.push_back({VA.getLocReg(), Arg});
    } else {
      assert(VA.isMemLoc());
      // Offset already includes the 32-byte pre-reservation.
      SDValue Addr = DAG.getNode(ISD::ADD, DL, PtrVT, SP,
                                  DAG.getConstant(VA.getLocMemOffset(), DL, PtrVT));
      MemOpChains.push_back(
          DAG.getStore(Chain, DL, Arg, Addr, MachinePointerInfo()));
    }
  }

  if (!MemOpChains.empty())
    Chain = DAG.getNode(ISD::TokenFactor, DL, MVT::Other, MemOpChains);

  // ---- Copy register args and attach glue --------------------------------
  SDValue Glue;
  for (auto &[Reg, Val] : RegsToPass) {
    Chain = DAG.getCopyToReg(Chain, DL, Reg, Val, Glue);
    Glue  = Chain.getValue(1);
  }

  // ---- Resolve callee address --------------------------------------------
  // Use CallVT (i64) so the callee node is typed as a GPR-width value.
  // The CALL_I instruction encoder uses only bits [31:0] of the address.
  if (auto *GA = dyn_cast<GlobalAddressSDNode>(Callee))
    Callee = DAG.getTargetGlobalAddress(GA->getGlobal(), DL, CallVT, 0);
  else if (auto *ES = dyn_cast<ExternalSymbolSDNode>(Callee))
    Callee = DAG.getTargetExternalSymbol(ES->getSymbol(), CallVT);

  // ---- Build the call node -----------------------------------------------
  SmallVector<SDValue, 16> Ops;
  Ops.push_back(Chain);
  Ops.push_back(Callee);
  for (auto &[Reg, Val] : RegsToPass)
    Ops.push_back(DAG.getRegister(Reg, Val.getValueType()));
  // Keep SP live-in to the call node so the scheduler doesn't move stores.
  Ops.push_back(DAG.getRegister(KlaussCPU::SP, MVT::i32));
  if (Glue.getNode())
    Ops.push_back(Glue);

  SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);
  Chain = DAG.getNode(KlaussCPUISD::CALL, DL, NodeTys, Ops);
  Glue  = Chain.getValue(1);

  Chain = DAG.getCALLSEQ_END(Chain, StackSize, 0, Glue, DL);
  Glue  = Chain.getValue(1);

  // ---- Copy return values from R12 ---------------------------------------
  SmallVector<CCValAssign, 4> RVLocs;
  CCState RetCCInfo(CallConv, IsVarArg, MF, RVLocs, *DAG.getContext());
  RetCCInfo.AnalyzeCallResult(CLI.Ins, RetCC_KlaussCPU);

  for (const CCValAssign &VA : RVLocs) {
    assert(VA.isRegLoc() && "KlaussCPU: only register returns supported");
    SDValue RV = DAG.getCopyFromReg(Chain, DL, VA.getLocReg(), VA.getValVT(),
                                     Glue);
    InVals.push_back(RV.getValue(0));
    Chain = RV.getValue(1);
    Glue  = RV.getValue(2);
  }

  return Chain;
}
