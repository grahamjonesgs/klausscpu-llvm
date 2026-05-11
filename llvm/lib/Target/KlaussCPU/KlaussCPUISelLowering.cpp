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
#include "KlaussCPUMachineFunctionInfo.h"
#include "KlaussCPURegisterInfo.h"
#include "KlaussCPUSubtarget.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "KlaussCPUInstrInfo.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/Support/ErrorHandling.h"
#include <optional>

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

  // ---- No CMOV: SELECT → Custom (emits SELECT_PSEUDO → branches via
  //   EmitInstrWithCustomInserter).  SELECT_CC → Expand which lowers to
  //   SETCC + SELECT; that SELECT then goes through our Custom handler.
  //   Both SELECT and SELECT_CC must NOT both be Expand — the legalizer
  //   asserts if SELECT_CC tries to expand via SELECT that is also Expand.
  setOperationAction(ISD::SELECT,    MVT::i64, Custom);
  setOperationAction(ISD::SELECT_CC, MVT::i64, Expand);

  // ---- Sub-word loads / stores ----
  // Hardware has LDIDX8/16/32 (zero-extending, base+offset) and
  // LDIDX8_S/LDIDX16_S (sign-extending, base+offset).  No sign-extending i32 load.
  //
  // SEXTLOAD i8/i16 → Legal (LDIDX8_S / LDIDX16_S hardware instructions).
  // SEXTLOAD i32   → Expand → ZEXTLOAD + SIGN_EXTEND_INREG (SEXTW).
  // SIGN_EXTEND_INREG i8/i16 → Legal (SEXTB/SEXTH hardware instructions).
  // SIGN_EXTEND_INREG i32 → Legal (SEXTW hardware instruction, fixed April 2026).
  // i1 loads: GlobalOpt at -O1 can shrink a bool-valued global to i1; promote to
  // i8 so the existing MEMGET8/LDIDX8 paths handle it.
  for (MVT ExtVT : {MVT::i64, MVT::i32})
    for (ISD::LoadExtType ET : {ISD::ZEXTLOAD, ISD::EXTLOAD, ISD::SEXTLOAD})
      setLoadExtAction(ET, ExtVT, MVT::i1, Promote);
  for (MVT VT : {MVT::i8, MVT::i16, MVT::i32}) {
    setLoadExtAction(ISD::ZEXTLOAD, MVT::i64, VT, Legal);
    setLoadExtAction(ISD::EXTLOAD,  MVT::i64, VT, Legal);
    setLoadExtAction(ISD::SEXTLOAD, MVT::i64, VT, Expand);
    setLoadExtAction(ISD::SEXTLOAD, MVT::i32, VT, Expand);
  }
  setLoadExtAction(ISD::SEXTLOAD, MVT::i64, MVT::i8,  Legal);
  setLoadExtAction(ISD::SEXTLOAD, MVT::i64, MVT::i16, Legal);
  setTruncStoreAction(MVT::i64, MVT::i8,  Legal);
  setTruncStoreAction(MVT::i64, MVT::i16, Legal);
  setTruncStoreAction(MVT::i64, MVT::i32, Legal);
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i1,  Expand);
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i8,  Legal);
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i16, Legal);
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i32, Legal);

  // ---- No atomics ----
  setMaxAtomicSizeInBitsSupported(0);

  // ---- Stack operations ----
  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i64, Expand);
  setOperationAction(ISD::STACKSAVE,          MVT::Other, Custom);
  setOperationAction(ISD::STACKRESTORE,       MVT::Other, Custom);

  // ---- Global addresses / symbols ----
  // All symbols are 32-bit addresses (128 MiB RAM); lowered to TargetGlobalAddress
  // which the isel pattern maps to SETR (sign-extends; bit 31 is always 0 for
  // valid addresses, so sign-ext == zero-ext).
  setOperationAction(ISD::GlobalAddress,  MVT::i64, Custom);
  setOperationAction(ISD::ExternalSymbol, MVT::i64, Custom);
  setOperationAction(ISD::JumpTable,      MVT::i64, Custom);

  // ---- Branch/control ----
  // JMPR is available.  Jump tables use EK_LabelDifference32 (4-byte entries
  // storing target-base offsets) — safe for ELFCLASS32.  BR_JT is Custom:
  // LowerBR_JT loads the 32-bit offset and adds the table base, then BRIND.
  setOperationAction(ISD::BR_JT,  MVT::Other, Custom);
  setOperationAction(ISD::BRIND,  MVT::Other, Legal);
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

  // No hardware rotate — expand to (shl | lshr) pairs.
  setOperationAction(ISD::ROTL, MVT::i64, Expand);
  setOperationAction(ISD::ROTR, MVT::i64, Expand);
  setOperationAction(ISD::ROTL, MVT::i32, Expand);
  setOperationAction(ISD::ROTR, MVT::i32, Expand);

  // No 128-bit multiply node — expand UMUL_LOHI/SMUL_LOHI to MUL + MULHU/MULHS.
  setOperationAction(ISD::UMUL_LOHI, MVT::i64, Expand);
  setOperationAction(ISD::SMUL_LOHI, MVT::i64, Expand);

  // 128-bit shift halves (from __uint128_t arithmetic in e.g. Ryu).
  // Lowered to pairs of i64 shifts + SELECT.
  setOperationAction(ISD::SHL_PARTS, MVT::i64, Custom);
  setOperationAction(ISD::SRL_PARTS, MVT::i64, Custom);
  setOperationAction(ISD::SRA_PARTS, MVT::i64, Custom);

  setOperationAction(ISD::TRAP, MVT::Other, Legal);

  // ---- Varargs ----
  // VASTART stores the address of the first variadic argument into the va_list.
  // VAARG/VACOPY/VAEND are handled inline by Clang for VoidPtrBuiltinVaList.
  setOperationAction(ISD::VASTART, MVT::Other, Custom);
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
  case ISD::JumpTable:      return LowerJumpTable(Op, DAG);
  case ISD::BR_JT:          return LowerBR_JT(Op, DAG);
  case ISD::SELECT:         return LowerSELECT(Op, DAG);
  case ISD::STACKSAVE:      return LowerSTACKSAVE(Op, DAG);
  case ISD::STACKRESTORE:   return LowerSTACKRESTORE(Op, DAG);
  case ISD::VASTART:        return LowerVASTART(Op, DAG);
  case ISD::SHL_PARTS:      return LowerShiftLeftParts(Op, DAG);
  case ISD::SRL_PARTS:      return LowerShiftRightParts(Op, DAG, /*IsSRA=*/false);
  case ISD::SRA_PARTS:      return LowerShiftRightParts(Op, DAG, /*IsSRA=*/true);
  default:
    llvm_unreachable("KlaussCPU: unimplemented LowerOperation opcode");
  }
}

SDValue KlaussCPUTargetLowering::LowerShiftLeftParts(SDValue Op,
                                                       SelectionDAG &DAG) const {
  SDLoc DL(Op);
  SDValue Lo = Op.getOperand(0);
  SDValue Hi = Op.getOperand(1);
  SDValue Shamt = Op.getOperand(2);
  EVT VT = MVT::i64;

  // if (Shamt - 64) < 0:   // Shamt < 64
  //   new_Lo = Lo << Shamt
  //   new_Hi = (Hi << Shamt) | (Lo >> 1 >> (63 - Shamt))
  // else:
  //   new_Lo = 0
  //   new_Hi = Lo << (Shamt - 64)

  SDValue Zero       = DAG.getConstant(0, DL, VT);
  SDValue One        = DAG.getConstant(1, DL, VT);
  SDValue Minus64    = DAG.getSignedConstant(-64, DL, VT);
  SDValue SixtyThree = DAG.getConstant(63, DL, VT);
  SDValue ShamtMinus64  = DAG.getNode(ISD::ADD, DL, VT, Shamt, Minus64);
  SDValue SixtyThreeMShamt = DAG.getNode(ISD::SUB, DL, VT, SixtyThree, Shamt);

  SDValue LoTrue       = DAG.getNode(ISD::SHL, DL, VT, Lo, Shamt);
  SDValue ShiftRight1Lo = DAG.getNode(ISD::SRL, DL, VT, Lo, One);
  SDValue ShiftRightLo  = DAG.getNode(ISD::SRL, DL, VT, ShiftRight1Lo, SixtyThreeMShamt);
  SDValue ShiftLeftHi   = DAG.getNode(ISD::SHL, DL, VT, Hi, Shamt);
  SDValue HiTrue  = DAG.getNode(ISD::OR,  DL, VT, ShiftLeftHi, ShiftRightLo);
  SDValue HiFalse = DAG.getNode(ISD::SHL, DL, VT, Lo, ShamtMinus64);

  SDValue CC = DAG.getSetCC(DL, VT, ShamtMinus64, Zero, ISD::SETLT);
  Lo = DAG.getNode(ISD::SELECT, DL, VT, CC, LoTrue, Zero);
  Hi = DAG.getNode(ISD::SELECT, DL, VT, CC, HiTrue, HiFalse);

  SDValue Parts[2] = {Lo, Hi};
  return DAG.getMergeValues(Parts, DL);
}

SDValue KlaussCPUTargetLowering::LowerShiftRightParts(SDValue Op,
                                                        SelectionDAG &DAG,
                                                        bool IsSRA) const {
  SDLoc DL(Op);
  SDValue Lo = Op.getOperand(0);
  SDValue Hi = Op.getOperand(1);
  SDValue Shamt = Op.getOperand(2);
  EVT VT = MVT::i64;

  // if (Shamt - 64) < 0:   // Shamt < 64
  //   new_Lo = (Lo >> Shamt) | ((Hi << 1) << (63 - Shamt))
  //   new_Hi = Hi >> Shamt  (arithmetic for SRA, logical for SRL)
  // else:
  //   new_Lo = Hi >> (Shamt - 64)  (arithmetic for SRA, logical for SRL)
  //   new_Hi = IsSRA ? Hi >> 63 : 0

  unsigned ShiftRightOp = IsSRA ? ISD::SRA : ISD::SRL;

  SDValue Zero       = DAG.getConstant(0, DL, VT);
  SDValue One        = DAG.getConstant(1, DL, VT);
  SDValue Minus64    = DAG.getSignedConstant(-64, DL, VT);
  SDValue SixtyThree = DAG.getConstant(63, DL, VT);
  SDValue ShamtMinus64    = DAG.getNode(ISD::ADD, DL, VT, Shamt, Minus64);
  SDValue SixtyThreeMShamt = DAG.getNode(ISD::SUB, DL, VT, SixtyThree, Shamt);

  SDValue ShiftRightLo = DAG.getNode(ISD::SRL, DL, VT, Lo, Shamt);
  SDValue ShiftLeftHi1 = DAG.getNode(ISD::SHL, DL, VT, Hi, One);
  SDValue ShiftLeftHi  = DAG.getNode(ISD::SHL, DL, VT, ShiftLeftHi1, SixtyThreeMShamt);
  SDValue LoTrue  = DAG.getNode(ISD::OR,          DL, VT, ShiftRightLo, ShiftLeftHi);
  SDValue HiTrue  = DAG.getNode(ShiftRightOp,     DL, VT, Hi, Shamt);
  SDValue LoFalse = DAG.getNode(ShiftRightOp,     DL, VT, Hi, ShamtMinus64);
  SDValue HiFalse = IsSRA ? DAG.getNode(ISD::SRA, DL, VT, Hi, SixtyThree) : Zero;

  SDValue CC = DAG.getSetCC(DL, VT, ShamtMinus64, Zero, ISD::SETLT);
  Lo = DAG.getNode(ISD::SELECT, DL, VT, CC, LoTrue, LoFalse);
  Hi = DAG.getNode(ISD::SELECT, DL, VT, CC, HiTrue, HiFalse);

  SDValue Parts[2] = {Lo, Hi};
  return DAG.getMergeValues(Parts, DL);
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

SDValue KlaussCPUTargetLowering::LowerJumpTable(SDValue Op,
                                                  SelectionDAG &DAG) const {
  auto *N = cast<JumpTableSDNode>(Op);
  SDValue TJT = DAG.getTargetJumpTable(N->getIndex(), MVT::i64);
  return DAG.getNode(KlaussCPUISD::ADDR, SDLoc(N), MVT::i64, TJT);
}

// Jump tables use EK_Custom32: each 4-byte entry holds the absolute 32-bit
// target address.  The assembler emits R_KLAUSSCPU_ABS32 per entry; the
// linker fills them in regardless of which ELF section the table lands in.
// Dispatch: load 4-byte entry (absolute address), zero-extend, BRIND.
SDValue KlaussCPUTargetLowering::LowerBR_JT(SDValue Op,
                                              SelectionDAG &DAG) const {
  SDValue Chain = Op.getOperand(0);
  SDValue Table = Op.getOperand(1);
  SDValue Index = Op.getOperand(2);
  SDLoc DL(Op);

  auto *JT = cast<JumpTableSDNode>(Table);
  SDValue Base = DAG.getNode(KlaussCPUISD::ADDR, DL, MVT::i64,
                              DAG.getTargetJumpTable(JT->getIndex(), MVT::i64));

  // EntryAddr = Base + Index * 4
  SDValue Scaled = DAG.getNode(ISD::SHL, DL, MVT::i64,
                                Index, DAG.getConstant(2, DL, MVT::i64));
  SDValue EntryAddr = DAG.getNode(ISD::ADD, DL, MVT::i64, Base, Scaled);

  // Load the 4-byte absolute target address from the table (zero-extend).
  // All code addresses fit in 32 bits, so zero-extension is correct.
  SDValue Target = DAG.getExtLoad(ISD::ZEXTLOAD, DL, MVT::i64, Chain,
                                   EntryAddr, MachinePointerInfo(), MVT::i32);

  return DAG.getNode(ISD::BRIND, DL, MVT::Other,
                     Target.getValue(1), Target.getValue(0));
}

// EK_Custom32 entry: emit the basic block's absolute address as an MCExpr.
// The assembler produces an R_KLAUSSCPU_ABS32 relocation; the linker resolves
// it to the final 32-bit address of the target basic block.
const MCExpr *KlaussCPUTargetLowering::LowerCustomJumpTableEntry(
    const MachineJumpTableInfo *, const MachineBasicBlock *MBB,
    unsigned, MCContext &Ctx) const {
  return MCSymbolRefExpr::create(MBB->getSymbol(), Ctx);
}

unsigned KlaussCPUTargetLowering::getJumpTableEncoding() const {
  return MachineJumpTableInfo::EK_Custom32;
}

std::pair<unsigned, const TargetRegisterClass *>
KlaussCPUTargetLowering::getRegForInlineAsmConstraint(
    const TargetRegisterInfo *TRI, StringRef Constraint, MVT VT) const {
  if (Constraint.size() == 1) {
    switch (Constraint[0]) {
    case 'r':
      return {0, &KlaussCPU::GPRRegClass};
    }
  }
  return TargetLowering::getRegForInlineAsmConstraint(TRI, Constraint, VT);
}

// __builtin_stack_save() / __builtin_stack_restore() support.
// SP is a 32-bit register; promote to/from pointer width (i64) as needed.

SDValue KlaussCPUTargetLowering::LowerSTACKSAVE(SDValue Op,
                                                  SelectionDAG &DAG) const {
  SDLoc DL(Op);
  SDValue Chain = Op.getOperand(0);
  // Use GETSP_R directly to avoid CopyFromReg(SP, i32) which fails
  // type legalization (SP has no legal i32 register class path).
  SDValue SP64 = SDValue(DAG.getMachineNode(KlaussCPU::GETSP_R, DL, MVT::i64), 0);
  return DAG.getMergeValues({SP64, Chain}, DL);
}

SDValue KlaussCPUTargetLowering::LowerSTACKRESTORE(SDValue Op,
                                                     SelectionDAG &DAG) const {
  SDLoc DL(Op);
  SDValue Chain   = Op.getOperand(0);
  SDValue NewSP64 = Op.getOperand(1);
  // SETSP_R takes a 64-bit GPR and writes bits[31:0] to SP.
  // Use it directly to avoid TRUNCATE i64→i32 and CopyToReg(SP, i32)
  // which both hit type-legalization issues.
  SDNode *SetSP = DAG.getMachineNode(KlaussCPU::SETSP_R, DL, MVT::Other,
                                      {NewSP64, Chain});
  return SDValue(SetSP, 0);
}

// va_start(ap, last_named) — store the address of the first variadic argument
// into *ap.  With VoidPtrBuiltinVaList, ap is just a void * that va_arg bumps.
SDValue KlaussCPUTargetLowering::LowerVASTART(SDValue Op,
                                               SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  int VaFI = MF.getInfo<KlaussCPUMachineFunctionInfo>()->getVarArgsFrameIndex();

  SDLoc DL(Op);
  SDValue FIN = DAG.getFrameIndex(VaFI, getPointerTy(DAG.getDataLayout()));

  // Store the frame address (= pointer to first vararg slot) into *ap.
  const Value *SV = cast<SrcValueSDNode>(Op.getOperand(2))->getValue();
  return DAG.getStore(Op.getOperand(0), DL, FIN, Op.getOperand(1),
                       MachinePointerInfo(SV));
}

const char *
KlaussCPUTargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch (Opcode) {
  case KlaussCPUISD::RET_GLUE: return "KlaussCPUISD::RET_GLUE";
  case KlaussCPUISD::CALL:     return "KlaussCPUISD::CALL";
  case KlaussCPUISD::ADDR:     return "KlaussCPUISD::ADDR";
  case KlaussCPUISD::SELECT:   return "KlaussCPUISD::SELECT";
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
      // Offset is relative to incoming SP (≥ 32 due to pre-reserve).
      // eliminateFrameIndex adds 8 for fixed objects (R15 = incoming_SP - 8).
      int FI = MF.getFrameInfo().CreateFixedObject(8, VA.getLocMemOffset(),
                                                    /*isImmutable=*/true);
      SDValue FIN = DAG.getFrameIndex(FI, getPointerTy(DAG.getDataLayout()));
      InVals.push_back(DAG.getLoad(MVT::i64, DL, Chain, FIN,
                                    MachinePointerInfo::getFixedStack(MF, FI)));
    }
  }

  if (IsVarArg) {
    // Argument registers R0–R3.  C requires at least one named parameter before
    // '...', so the first unallocated register is R1 at the earliest.
    static const MCPhysReg ArgRegs[] = {
        KlaussCPU::R0, KlaussCPU::R1, KlaussCPU::R2, KlaussCPU::R3};
    constexpr unsigned NumArgRegs = 4;

    // Index of the first unallocated (= first potential vararg) register.
    unsigned FirstVaReg =
        CCInfo.getFirstUnallocated(ArrayRef<MCPhysReg>(ArgRegs, NumArgRegs));

    auto &MFI = MF.getFrameInfo();

    // Arg register R_I sits in the caller's "reserved area" at [incoming_SP + I*8].
    // We save R_{FirstVaReg}..R3 there so they are contiguous with stack args at
    // [incoming_SP+32..], allowing va_arg to advance a plain pointer through all args.
    //
    // eliminateFrameIndex adds 8 for fixed objects (R15 = incoming_SP - 8), so
    // CreateFixedObject with offset F yields the correct R15-relative offset F+8.

    std::optional<int> VaArgFI;
    SmallVector<SDValue, 4> Stores;
    for (unsigned I = FirstVaReg; I < NumArgRegs; ++I) {
      int SlotOff = (int)(I * 8); // R_I → [incoming_SP + I*8]
      int FI = MFI.CreateFixedObject(8, SlotOff, /*isImmutable=*/false);
      if (!VaArgFI)
        VaArgFI = FI;
      Register VReg = RegInfo.createVirtualRegister(&KlaussCPU::GPRRegClass);
      RegInfo.addLiveIn(ArgRegs[I], VReg);
      SDValue FIN = DAG.getFrameIndex(FI, getPointerTy(DAG.getDataLayout()));
      SDValue Val = DAG.getCopyFromReg(Chain, DL, VReg, MVT::i64);
      Stores.push_back(DAG.getStore(Chain, DL, Val, FIN,
                                     MachinePointerInfo::getFixedStack(MF, FI)));
    }
    if (!Stores.empty())
      Chain = DAG.getNode(ISD::TokenFactor, DL, MVT::Other, Stores);

    // If all registers were used for named args, the first vararg is on the stack.
    if (!VaArgFI)
      VaArgFI = MFI.CreateFixedObject(8, (int)CCInfo.getStackSize(),
                                       /*isImmutable=*/false);

    MF.getInfo<KlaussCPUMachineFunctionInfo>()->setVarArgsFrameIndex(*VaArgFI);
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
  // KlaussCPU has no tail-call optimization — our calling convention uses a
  // dedicated CALL instruction that saves LR, so TCO would need a special
  // JMP-to-callee sequence we haven't implemented.
  CLI.IsTailCall = false;

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
  // CALL pre-decrements SP by 8 (stores return address at [SP-8], SP -= 8).
  // So incoming_SP = CallerSP - 8.  From the caller's GETSP_R perspective:
  //   [GETSP+0..7]   = return address slot  (== incoming_SP - 8..+7 )
  //   [GETSP+0..23]  = reserved area for R1-R3 saves (incoming_SP +8..+31)
  //   [GETSP+24..]   = stack args (incoming_SP +32 onwards)
  // The callee pre-reserves 32 bytes from incoming_SP.  The caller's GETSP
  // value is incoming_SP + 8, so pre-reserve only 24 bytes here so the first
  // stack arg offset (24) maps to incoming_SP + 32 on the callee side.
  CCInfo.AllocateStack(24, Align(8));
  CCInfo.AnalyzeCallOperands(CLI.Outs, CC_KlaussCPU);

  unsigned StackSize = CCInfo.getStackSize(); // ≥ 32

  Chain = DAG.getCALLSEQ_START(Chain, StackSize, 0, DL);

  // ---- Collect register and memory args ----------------------------------
  SmallVector<std::pair<Register, SDValue>, 8> RegsToPass;
  SmallVector<SDValue, 4> MemOpChains;

  // Use GETSP_R to read the 32-bit SP into an i64 GPR (zero-extends).
  // Avoid getCopyFromReg(SP, i32): the type legalizer cannot promote
  // i32 CopyFromReg nodes for physical-only registers (no i32 reg class).
  SDValue SP = SDValue(DAG.getMachineNode(KlaussCPU::GETSP_R, DL, PtrVT), 0);

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

// SELECT: no hardware CMOV — emit a KlaussCPUISD::SELECT node which is
// selected to SELECT_PSEUDO and then expanded to branches by
// EmitInstrWithCustomInserter below.
SDValue KlaussCPUTargetLowering::LowerSELECT(SDValue Op,
                                               SelectionDAG &DAG) const {
  return DAG.getNode(KlaussCPUISD::SELECT, SDLoc(Op), Op.getValueType(),
                     Op.getOperand(0), Op.getOperand(1), Op.getOperand(2));
}

// Expand SELECT_PSEUDO to a diamond with a PHI at the sink:
//
//   TestBB:  CMPRV cond, 0
//            JMPE  SinkBB      ← cond==0 → false path (directly to sink)
//            (fall through to TrueBB)
//   TrueBB:  (empty, falls through to SinkBB)
//   SinkBB:  DstReg = PHI [ FalseReg, TestBB ], [ TrueReg, TrueBB ]
//            ... (rest of original BB) ...
//
// This preserves SSA: DstReg has exactly one definition (the PHI).
// FalseReg and TrueReg are defined in the original BB which dominates all paths.
MachineBasicBlock *KlaussCPUTargetLowering::EmitInstrWithCustomInserter(
    MachineInstr &MI, MachineBasicBlock *BB) const {

  assert(MI.getOpcode() == KlaussCPU::SELECT_PSEUDO);

  const KlaussCPUInstrInfo &TII =
      *BB->getParent()->getSubtarget<KlaussCPUSubtarget>().getInstrInfo();
  DebugLoc DL = MI.getDebugLoc();
  MachineFunction *MF = BB->getParent();
  const BasicBlock *LLVMBB = BB->getBasicBlock();

  Register DstReg   = MI.getOperand(0).getReg();
  Register CondReg  = MI.getOperand(1).getReg();
  Register TrueReg  = MI.getOperand(2).getReg();
  Register FalseReg = MI.getOperand(3).getReg();

  MachineBasicBlock *TrueBB = MF->CreateMachineBasicBlock(LLVMBB);
  MachineBasicBlock *SinkBB = MF->CreateMachineBasicBlock(LLVMBB);

  MachineFunction::iterator It = ++BB->getIterator();
  MF->insert(It, TrueBB);
  MF->insert(It, SinkBB);

  // Move the tail of BB (after the SELECT_PSEUDO) into SinkBB.
  SinkBB->splice(SinkBB->begin(), BB,
                 std::next(MI.getIterator()), BB->end());
  SinkBB->transferSuccessorsAndUpdatePHIs(BB);

  // TestBB: compare cond with 0; if equal (cond==0) jump to SinkBB (false path),
  // otherwise fall through to TrueBB.
  BuildMI(BB, DL, TII.get(KlaussCPU::CMPRV_I)).addReg(CondReg).addImm(0);
  BuildMI(BB, DL, TII.get(KlaussCPU::JMPE)).addMBB(SinkBB);
  // Use explicit 50/50 probabilities: addSuccessor() with unknown probability
  // leaves Probs empty while Successors grows, causing an assertion in any
  // pass that reads branch probabilities (MachineBlockPlacement, BranchFolder).
  // addSuccessorWithoutProb: puts the block in "no probability tracking" mode.
  // Using explicit probabilities would cause the branch folder to crash when
  // it later calls addSuccessor(Unknown) on the same block, creating a
  // Probs.size() != Successors.size() mismatch.
  BB->addSuccessorWithoutProb(SinkBB); // false path (JMPE taken)
  BB->addSuccessorWithoutProb(TrueBB); // true path  (fall through)

  // TrueBB: empty, just falls through to SinkBB.
  TrueBB->addSuccessorWithoutProb(SinkBB);

  // SinkBB: PHI merges the two incoming values; DstReg has exactly one def.
  BuildMI(*SinkBB, SinkBB->begin(), DL, TII.get(TargetOpcode::PHI), DstReg)
      .addReg(FalseReg).addMBB(BB)     // from TestBB when JMPE taken (cond==0)
      .addReg(TrueReg).addMBB(TrueBB); // from TrueBB when cond!=0

  MI.eraseFromParent();
  return SinkBB;
}
