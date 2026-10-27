//===-- KlaussCPUISelLowering.cpp - KlaussCPU DAG Lowering ----------------===//
//
// KlaussCPU LLVM backend — SelectionDAG lowering implementation.
//
// Key architecture constraints:
//   - No hardware FP               → soft-float
//   - No CMOV                      → SELECT must be expanded
//   - No atomic instructions       → forced atomics: atomic load/store
//                                    ≤ 8 bytes are plain loads/stores,
//                                    RMW/cmpxchg are __sync_* libcalls
//   - Sign-extending loads         → LDIDX8_S / LDIDX16_S / LDIDX32_S
//   - Hardware integer ops         → div, mul-high, min/max, setcc are Legal
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUISelLowering.h"
#include "KlaussCPUMachineFunctionInfo.h"
#include "KlaussCPURegisterInfo.h"
#include "KlaussCPUSubtarget.h"
#include "llvm/ADT/SmallSet.h"
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
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include <optional>

using namespace llvm;

// Loop-header alignment, in bytes.  Defaults to the 16-byte I-cache line so a
// hot loop's back-edge target starts a line (see setPrefLoopAlignment below).
// 0 disables alignment entirely, restoring the pre-M8b baseline for a clean A/B.
static cl::opt<unsigned>
    PrefLoopAlign("klausscpu-pref-loop-align", cl::Hidden, cl::init(16),
                  cl::desc("Preferred loop-header alignment in bytes "
                           "(16 = I-cache line; 0 = off)."));

// Jump tables (LowerBR_JT).  A dispatch through the table costs about the
// same whatever the switch's size, a compare tree grows with it; on the board
// the table breaks even at 12 cases (see the constructor).  0 disables them.
static cl::opt<unsigned> MinJumpTableEntries(
    "klausscpu-min-jump-table-entries", cl::Hidden, cl::init(12),
    cl::desc("Minimum cases for a jump table (0 = jump tables disabled)."));

// The share of a table's entries that must be cases.  A 4-byte entry against
// one or two instruction words per case of a compare tree: below about 40%
// the table is the bigger of the two.
static cl::opt<unsigned> JumpTableDensity(
    "klausscpu-jump-table-density", cl::Hidden, cl::init(40),
    cl::desc("Minimum percentage of a jump table's entries that are cases."));

// A run of selects on the same condition shares one compare and branch (see
// EmitInstrWithCustomInserter); off gives each select its own diamond.
static cl::opt<bool> GroupSelects(
    "klausscpu-group-selects", cl::Hidden, cl::init(true),
    cl::desc("Share one compare and branch between consecutive selects on "
             "the same condition"));

// decomposeMulByConstant: also trade a MULV for a shift and an add/sub when
// the schedule model makes the pair faster (one more instruction, the same
// size).
static cl::opt<bool> MulImmShiftAdd(
    "klausscpu-mul-imm-shift-add", cl::Hidden, cl::init(false),
    cl::desc("Expand a multiply by a simm32 constant 2^N+1, 2^N-1 or 1-2^N "
             "into a shift and an add/sub when the schedule model rates "
             "MULV slower"));

// Generated calling convention functions.  Must be included inside namespace
// llvm because the generated code uses unqualified names (MVT, CCState, etc.).
namespace llvm {
#include "KlaussCPUGenCallingConv.inc"
} // namespace llvm

KlaussCPUTargetLowering::KlaussCPUTargetLowering(const TargetMachine &TM,
                                                   const KlaussCPUSubtarget &STI)
    : TargetLowering(TM, STI), Subtarget(STI) {

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

  // ---- No CMOV: SELECT and SELECT_CC → Custom, both lowered to
  //   KlaussCPUISD::SELECT_CC (lhs, rhs, cc, t, f).  That selects to the
  //   SELECT_CC_RR/RI pseudo, which EmitInstrWithCustomInserter expands to
  //   one CMPRR/CMPRV + JMPcc + PHI.  Folding the compare into the select
  //   avoids materialising a 0/1 with CMPxxR and re-testing it with
  //   CMPRV 0 + JMPE (saves 3 words and an ALU op per select).
  setOperationAction(ISD::SELECT,    MVT::i64, Custom);
  setOperationAction(ISD::SELECT_CC, MVT::i64, Custom);

  // ---- Sub-word loads / stores ----
  // Hardware has LDIDX8/16/32 (zero-extending, base+offset) and
  // LDIDX8_S/LDIDX16_S (sign-extending, base+offset).  No sign-extending i32 load.
  //
  // SEXTLOAD i8/i16 → Legal (LDIDX8_S / LDIDX16_S hardware instructions).
  // SEXTLOAD i32   → Legal (LDIDX32_S, ISA v3 D1; was ZEXTLOAD + SEXTW).
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
  setLoadExtAction(ISD::SEXTLOAD, MVT::i64, MVT::i32, Legal);
  setTruncStoreAction(MVT::i64, MVT::i8,  Legal);
  setTruncStoreAction(MVT::i64, MVT::i16, Legal);
  setTruncStoreAction(MVT::i64, MVT::i32, Legal);
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i1,  Expand);
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i8,  Legal);
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i16, Legal);
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i32, Legal);

  // ---- Atomics: "forced atomics", as RISC-V +forced-atomics ----
  // The ISA has no atomic RMW, CAS or LL/SC.  Up to 64 bits:
  //  - a naturally aligned atomic load/store is a plain load/store (patterns
  //    in KlaussCPUInstrInfo.td): the in-order core makes every access
  //    visible in program order, also to its own interrupt handlers;
  //  - a fence is only a compiler barrier (LowerATOMIC_FENCE);
  //  - every RMW and cmpxchg is a __sync_*_N libcall, which the runtime
  //    implements with interrupts masked (klausscpu-runtime src/atomic.c).
  //    Atomic within one core only.
  // AtomicExpandPass turns larger or misaligned atomics into __atomic_*
  // libcalls and the RMWs without a __sync libcall into cmpxchg loops
  // (shouldExpandAtomicRMWInIR).  The libcalls pass sub-word values
  // any-extended and return them zero-extended (the default
  // getExtendForAtomicOps, which the runtime matches).
  setMaxAtomicSizeInBitsSupported(64);
  setOperationAction(ISD::ATOMIC_FENCE, MVT::Other, Custom);
  // The sign- and zero-extending loads are atomic too, so a [sz]ext of an
  // atomic load folds into it (LDIDXn_S / LDIDXn).
  setAtomicLoadExtAction({ISD::SEXTLOAD, ISD::ZEXTLOAD}, MVT::i64,
                         {MVT::i8, MVT::i16, MVT::i32}, Legal);
  setOperationAction({ISD::ATOMIC_CMP_SWAP, ISD::ATOMIC_SWAP,
                      ISD::ATOMIC_LOAD_ADD, ISD::ATOMIC_LOAD_SUB,
                      ISD::ATOMIC_LOAD_AND, ISD::ATOMIC_LOAD_OR,
                      ISD::ATOMIC_LOAD_XOR, ISD::ATOMIC_LOAD_NAND,
                      ISD::ATOMIC_LOAD_MIN, ISD::ATOMIC_LOAD_MAX,
                      ISD::ATOMIC_LOAD_UMIN, ISD::ATOMIC_LOAD_UMAX},
                     MVT::i64, LibCall);

  // ---- Stack operations ----
  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i64, Custom);
  setOperationAction(ISD::STACKSAVE,          MVT::Other, Custom);
  setOperationAction(ISD::STACKRESTORE,       MVT::Other, Custom);

  // ---- Global addresses / symbols ----
  // Non-PIC: GlobalAddress → ADDR → SETRU (absolute 32-bit address).
  // PIC:     GlobalAddress → LEAPC → LEAPC insn (PC-relative, PCREL32 fixup).
  setOperationAction(ISD::GlobalAddress,  MVT::i64, Custom);
  setOperationAction(ISD::ExternalSymbol, MVT::i64, Custom);
  setOperationAction(ISD::BlockAddress,   MVT::i64, Custom);
  setOperationAction(ISD::JumpTable,      MVT::i64, Custom);

  // ---- Branch/control ----
  // Jump tables from MinJumpTableEntries cases at JumpTableDensity.  The
  // dispatch (LowerBR_JT, the range check in front of it, and
  // KlaussCPUJumpTableHoist) is a load, its 2 load-use bubbles and a taken
  // JMPR, about as long as three or four levels of a compare tree.  Measured
  // on the board, a switch called with uniformly distributed keys: dense, the
  // table costs the same as the tree at 12 cases and saves 0.5 cycles a
  // dispatch at 13, 3.9 at 16, 5.6 at 20; at 50% density it saves 2.3 at 12.
  setMinimumJumpTableEntries(MinJumpTableEntries ? MinJumpTableEntries
                                                 : INT_MAX);
  setOperationAction(ISD::BR_JT,  MVT::Other, Custom); // LowerBR_JT
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
  // CMPEQR/CMPLTR/... write exactly 0 or 1.  Without this the DAG assumes the
  // upper bits of a setcc result are undefined and masks it with ANDV 1
  // before every use as an integer.
  setBooleanContents(ZeroOrOneBooleanContent);
  // (and (freeze (extload)), lowmask) → (freeze (zextload)); see
  // PerformDAGCombine.
  setTargetDAGCombine(ISD::AND);

  // Hardware 64-bit rotate (ROLR/RORR, ROLV/RORV).  There is no 32-bit
  // rotate, but a 64-bit rotate of a value whose two halves are equal is a
  // 32-bit rotate in each half: ReplaceNodeResults turns an i32 rotl/rotr
  // into ROLW/RORW, which select to that (3-4 instructions, the duplicated
  // value shared by every rotate of the same x, as in SHA-256's sigma
  // functions; the generic shift pair takes 4 for a constant amount and 7
  // for a variable one).
  setOperationAction(ISD::ROTL, MVT::i64, Legal);
  setOperationAction(ISD::ROTR, MVT::i64, Legal);
  setOperationAction(ISD::ROTL, MVT::i32, Custom);
  setOperationAction(ISD::ROTR, MVT::i32, Custom);

  // Hardware absolute value (ABSR).
  setOperationAction(ISD::ABS, MVT::i64, Legal);

  // Hardware bit-reverse (BITREV) — defaults to Expand (bswap + mask
  // sequence) unless declared Legal. CTPOP/BSWAP select already; explicit
  // for clarity.
  setOperationAction(ISD::BITREVERSE, MVT::i64, Legal);
  setOperationAction(ISD::CTPOP, MVT::i64, Legal);
  setOperationAction(ISD::BSWAP, MVT::i64, Legal);

  // CLZ(0) and CTZ(0) both return 64 in hardware (confirmed 2026-06-12),
  // matching LLVM's zero-defined ctlz/cttz semantics — so the zero-guard
  // compare/branch around u64::leading_zeros()/trailing_zeros() is not
  // needed and CTLZ/CTTZ are Legal, not just their ZERO_UNDEF variants.
  setOperationAction(ISD::CTLZ, MVT::i64, Legal);
  setOperationAction(ISD::CTTZ, MVT::i64, Legal);

  // No 128-bit multiply node — expand UMUL_LOHI/SMUL_LOHI to MUL + MULHU/MULHS.
  setOperationAction(ISD::UMUL_LOHI, MVT::i64, Expand);
  setOperationAction(ISD::SMUL_LOHI, MVT::i64, Expand);

  // 128-bit shift halves (from __uint128_t arithmetic in e.g. Ryu).
  // Lowered to pairs of i64 shifts + SELECT.
  setOperationAction(ISD::SHL_PARTS, MVT::i64, Custom);
  setOperationAction(ISD::SRL_PARTS, MVT::i64, Custom);
  setOperationAction(ISD::SRA_PARTS, MVT::i64, Custom);

  setOperationAction(ISD::TRAP, MVT::Other, Legal);
  setOperationAction(ISD::DEBUGTRAP, MVT::Other, Legal);

  // __builtin_frame_address / __builtin_return_address walk the R15 chain.
  setOperationAction(ISD::FRAMEADDR,  MVT::i64, Custom);
  setOperationAction(ISD::RETURNADDR, MVT::i64, Custom);

  // ---- Varargs ----
  // VASTART stores the address of the first variadic argument into the va_list.
  // VAARG/VACOPY/VAEND are handled inline by Clang for VoidPtrBuiltinVaList.
  setOperationAction(ISD::VASTART, MVT::Other, Custom);
  setOperationAction(ISD::VAARG,   MVT::Other, Custom);
  setOperationAction(ISD::VACOPY,  MVT::Other, Expand);
  setOperationAction(ISD::VAEND,   MVT::Other, Expand);

  // ---- Misc ----
  // An inline memcpy (a struct copy) as groups of four loads followed by
  // their four stores.  Ungrouped, isel emits load, store, load, store, and
  // nothing may move a load above the previous store (the scheduler cannot
  // tell the source from the destination), so every store waits for the
  // load just before it: two stall cycles per word (load-use latency 3,
  // KlaussCPUSchedule.td).  Grouped, each store is three or more
  // instructions after its load.
  MaxGluedStoresPerMemcpy = 4;

  // Only 32-bit and 64-bit native integer widths.
  setMinFunctionAlignment(Align(4));
  setPrefFunctionAlignment(Align(4));

  // Align loop headers to the 16-byte I-cache line.  Fetch works in aligned
  // 16-byte lines, so a back-edge target at offset 12 in a line makes the first
  // fetch of EVERY iteration deliver one useful instruction instead of four.
  // Aligning the header costs up to 3 NOPs once on loop entry (amortized to
  // nothing over a hot loop) and buys a full line of useful instructions per
  // iteration — it shows up as reduced fetch-idle (the counter perf_haz reports
  // as IFMISS, PERF_BASE+0xE0, which counts fetch starvation, not just misses).
  //
  // Not a cold-miss optimization: the compute kernels fit inside the 8 KB
  // I-cache and iterate ~20k times, so capacity/cold misses amortize away — this
  // targets the per-iteration line-straddle only.  It cannot help fetch-idle
  // caused by branch redirects, so expect little on branch-dense loops.
  //
  // A/B (spec §7.1): -klausscpu-pref-loop-align=0 restores the unaligned
  // baseline without a rebuild.  Padding is emitted by KlaussCPUAsmBackend::
  // writeNopData as v2 NOP 0x6C000000; it sits in the fall-through path and IS
  // executed, so that encoding must stay correct.
  if (PrefLoopAlign) {
    if (!isPowerOf2_32(PrefLoopAlign))
      report_fatal_error("-klausscpu-pref-loop-align must be a power of 2");
    setPrefLoopAlignment(Align(PrefLoopAlign));
  }
}

SDValue KlaussCPUTargetLowering::LowerOperation(SDValue Op,
                                                 SelectionDAG &DAG) const {
  switch (Op.getOpcode()) {
  case ISD::GlobalAddress:  return LowerGlobalAddress(Op, DAG);
  case ISD::ExternalSymbol: return LowerExternalSymbol(Op, DAG);
  case ISD::BlockAddress:   return LowerBlockAddress(Op, DAG);
  case ISD::JumpTable:      return LowerJumpTable(Op, DAG);
  case ISD::BR_JT:          return LowerBR_JT(Op, DAG);
  case ISD::SELECT:         return LowerSELECT(Op, DAG);
  case ISD::SELECT_CC:      return LowerSELECT_CC(Op, DAG);
  case ISD::STACKSAVE:           return LowerSTACKSAVE(Op, DAG);
  case ISD::STACKRESTORE:        return LowerSTACKRESTORE(Op, DAG);
  case ISD::DYNAMIC_STACKALLOC: return LowerDYNAMIC_STACKALLOC(Op, DAG);
  case ISD::VASTART:        return LowerVASTART(Op, DAG);
  case ISD::VAARG:          return LowerVAARG(Op, DAG);
  case ISD::SHL_PARTS:      return LowerShiftLeftParts(Op, DAG);
  case ISD::SRL_PARTS:      return LowerShiftRightParts(Op, DAG, /*IsSRA=*/false);
  case ISD::SRA_PARTS:      return LowerShiftRightParts(Op, DAG, /*IsSRA=*/true);
  case ISD::ATOMIC_FENCE:   return LowerATOMIC_FENCE(Op, DAG);
  case ISD::FRAMEADDR:      return LowerFRAMEADDR(Op, DAG);
  case ISD::RETURNADDR:     return LowerRETURNADDR(Op, DAG);
  default:
    report_fatal_error("KlaussCPU: unimplemented LowerOperation opcode");
  }
}

// The __sync libcalls cover xchg and the integer RMW ops (LibCall in the
// constructor); the others become a loop around cmpxchg, as in RISC-V.
TargetLowering::AtomicExpansionKind
KlaussCPUTargetLowering::shouldExpandAtomicRMWInIR(
    const AtomicRMWInst *AI) const {
  if (AI->isFloatingPointOperation() ||
      AI->getOperation() == AtomicRMWInst::UIncWrap ||
      AI->getOperation() == AtomicRMWInst::UDecWrap ||
      AI->getOperation() == AtomicRMWInst::USubCond ||
      AI->getOperation() == AtomicRMWInst::USubSat)
    return AtomicExpansionKind::CmpXChg;
  return AtomicExpansionKind::None;
}

bool KlaussCPUTargetLowering::isZExtFree(SDValue Val, EVT VT2) const {
  if (auto *LD = dyn_cast<LoadSDNode>(Val)) {
    EVT MemVT = LD->getMemoryVT();
    ISD::LoadExtType ET = LD->getExtensionType();
    if ((MemVT == MVT::i8 || MemVT == MVT::i16 || MemVT == MVT::i32) &&
        (ET == ISD::NON_EXTLOAD || ET == ISD::ZEXTLOAD || ET == ISD::EXTLOAD))
      return true;
  }
  return TargetLowering::isZExtFree(Val, VT2);
}

// The load/store addressing modes of KlaussCPUInstrInfo.td: [r] and
// r + simm32 at every width (MEMGETn/MEMSETn, LDIDXn/STIDXn), r + r with
// no displacement (LDIDXnR/STIDXnR, MODE=11), and a constant address that
// SETR or SETRU reaches, naturally aligned (LDABSn/STABSn, MEMREADR/MEMSETR,
// MODE=10).  There is no scaled index, and no symbol address: a global is a
// SETRU / LEAPC into a register first.  An access wider than a register is
// split into 8-byte pieces at base + 8k, which r + r does not reach.  (The
// default claims r + r for every size, any bare constant address and 2*r,
// and caps the offset at simm16.)
bool KlaussCPUTargetLowering::isLegalAddressingMode(const DataLayout &DL,
                                                    const AddrMode &AM,
                                                    Type *Ty, unsigned AS,
                                                    Instruction *I) const {
  if (AM.BaseGV || AM.ScalableOffset)
    return false;

  if (AM.Scale == 0 && !AM.HasBaseReg) { // a bare "i"
    uint64_t Bytes =
        Ty->isSized()
            ? std::min<uint64_t>(DL.getTypeStoreSize(Ty).getKnownMinValue(), 8)
            : 8;
    return (isInt<32>(AM.BaseOffs) || isUInt<32>(AM.BaseOffs)) &&
           isPowerOf2_64(Bytes) && AM.BaseOffs % Bytes == 0;
  }
  if (!isInt<32>(AM.BaseOffs))
    return false;

  switch (AM.Scale) {
  case 0: // "r + i"
    return AM.HasBaseReg;
  case 1:
    if (!AM.HasBaseReg) // "r + i"
      return true;
    // "r + r"
    return AM.BaseOffs == 0 &&
           (!Ty->isSized() || DL.getTypeStoreSize(Ty).getKnownMinValue() <= 8);
  default:
    return false;
  }
}

// ADDI adds a simm32, ADDV a zero-extended imm32, MINUSV subtracts one.
bool KlaussCPUTargetLowering::isLegalAddImmediate(int64_t Imm) const {
  return isInt<32>(Imm) || isUInt<32>(Imm) ||
         isUInt<32>(-static_cast<uint64_t>(Imm));
}

// A compare that feeds a branch is CMPRV (simm32) or CMPRVU (zero-extended
// imm32).  A setcc that produces a value is CMPxxV, which only has the simm32
// form: an imm32 above INT32_MAX goes in a register (SETRU + CMPxxR).  LSR
// asks this for its ICmpZero uses, the loop-exit compare and branch, so this
// describes the branch forms.
bool KlaussCPUTargetLowering::isLegalICmpImmediate(int64_t Imm) const {
  return isInt<32>(Imm) || isUInt<32>(Imm);
}

// The DAGCombiner calls this for (mul x, C) and, on true, expands C =
// 2^N + 1, 2^N - 1 or 1 - 2^N into (x << N) + x, (x << N) - x or
// x - (x << N): two ALU ops, both 1 word.  Only those shapes are accepted:
// -(2^N + 1) needs a NEGR and a constant with trailing zeros a second
// shift, each one more instruction.
//
// A constant that is not a simm32 has no MULV: it costs SETRU/SETR64 and
// MULR, 2 instructions and 3-4 words, so the 2-word pair is smaller and
// faster.  (In a loop MachineLICM may hoist the constant, leaving one MULR
// against the pair: one instruction more, but still fewer cycles.)  A
// simm32 constant is one 2-word MULV: the pair is the same size and one
// instruction more, so it is used only on request
// (-klausscpu-mul-imm-shift-add) and when the schedule model rates the
// multiply slower than the dependent shift and add (5 against 2 + 2 cycles
// in KlaussCPUSchedule.td).
bool KlaussCPUTargetLowering::decomposeMulByConstant(LLVMContext &Context,
                                                     EVT VT, SDValue C) const {
  if (!VT.isScalarInteger() || VT.getSizeInBits() > 64)
    return false;
  const APInt &Imm = cast<ConstantSDNode>(C)->getAPIntValue();
  // The combiner expands |C| (preferring the add form: 3 = 2 + 1 = 4 - 1)
  // and negates a negative C's result; only the sub form's negation folds,
  // into x - (x << N).
  APInt MulC = Imm.abs();
  bool AddForm = (MulC - 1).isPowerOf2();
  bool SubForm = (MulC + 1).isPowerOf2();
  if (Imm.isNegative() ? AddForm || !SubForm : !AddForm && !SubForm)
    return false;
  if (!Imm.isSignedIntN(32))
    return true;
  if (!MulImmShiftAdd)
    return false;
  const MCSchedModel &SM = Subtarget.getSchedModel();
  if (!SM.hasInstrSchedModel())
    return false;
  const KlaussCPUInstrInfo &TII = *Subtarget.getInstrInfo();
  auto Latency = [&](unsigned Opc) {
    return SM.computeInstrLatency(Subtarget, TII.get(Opc).getSchedClass());
  };
  return Latency(KlaussCPU::MULV) >
         Latency(KlaussCPU::SHLV) + Latency(KlaussCPU::ADDR);
}

// An add of C is one instruction while C is an imm32 that ADDI, ADDV or
// MINUSV takes; any other C needs a SETR64.
static bool isAddImmediate(const APInt &C) {
  return C.getBitWidth() <= 64 &&
         (C.isSignedIntN(32) || C.isIntN(32) || (-C).isIntN(32));
}

// (mul (add x, c1), c2) -> (add (mul x, c2), c1 * c2) is worse when c1 is an
// add immediate and c1 * c2 is not.
bool KlaussCPUTargetLowering::isMulAddWithConstProfitable(
    SDValue AddNode, SDValue ConstNode) const {
  EVT VT = AddNode.getValueType();
  if (VT.isVector() || VT.getScalarSizeInBits() > 64)
    return true;
  const APInt &C1 = AddNode.getConstantOperandAPInt(1);
  const APInt &C2 = cast<ConstantSDNode>(ConstNode)->getAPIntValue();
  return !isAddImmediate(C1) || isAddImmediate(C1 * C2);
}

void KlaussCPUTargetLowering::ReplaceNodeResults(
    SDNode *N, SmallVectorImpl<SDValue> &Results, SelectionDAG &DAG) const {
  switch (N->getOpcode()) {
  case ISD::ROTL:
  case ISD::ROTR: {
    // An i32 rotate: amounts are modulo 32 and so are ROLW/RORW's (the
    // CPU uses amount[5:0], and the duplicated value repeats every 32 bits).
    assert(N->getValueType(0) == MVT::i32 && "unexpected rotate type");
    SDLoc DL(N);
    SDValue X = DAG.getNode(ISD::ANY_EXTEND, DL, MVT::i64, N->getOperand(0));
    SDValue Amt =
        DAG.getNode(ISD::ANY_EXTEND, DL, MVT::i64, N->getOperand(1));
    unsigned Opc =
        N->getOpcode() == ISD::ROTL ? KlaussCPUISD::ROLW : KlaussCPUISD::RORW;
    SDValue Res = DAG.getNode(Opc, DL, MVT::i64, X, Amt);
    Results.push_back(DAG.getNode(ISD::TRUNCATE, DL, MVT::i32, Res));
    return;
  }
  default:
    llvm_unreachable("KlaussCPU: unexpected node to custom-legalize");
  }
}

SDValue KlaussCPUTargetLowering::PerformDAGCombine(SDNode *N,
                                                   DAGCombinerInfo &DCI) const {
  SelectionDAG &DAG = DCI.DAG;
  switch (N->getOpcode()) {
  case ISD::AND: {
    // (and (freeze (extload x)), lowmask) -> (freeze (zextload x))
    //
    // A promoted `int` compare zero-extends its operands with an AND mask.
    // The generic combine folds that mask into the load as a ZEXTLOAD (free:
    // every sub-i64 load zero-extends), but not when a FREEZE sits between
    // them — and SelectionDAG freezes the operands of short-circuit `a || b`
    // conditions (select i1 a, i1 b, false).  That left a redundant ZEXTW
    // after the load in e.g. the N-queens inner loop.  Masking a frozen
    // anyext load down to the memory width is exactly a frozen zextload.
    auto *Mask = dyn_cast<ConstantSDNode>(N->getOperand(1));
    SDValue Fr = N->getOperand(0);
    if (!Mask || Fr.getOpcode() != ISD::FREEZE)
      break;
    auto *LD = dyn_cast<LoadSDNode>(Fr.getOperand(0));
    if (!LD || LD->getExtensionType() != ISD::EXTLOAD || !LD->isUnindexed() ||
        !LD->isSimple() || LD->getValueType(0) != MVT::i64)
      break;
    EVT MemVT = LD->getMemoryVT();
    if ((MemVT != MVT::i8 && MemVT != MVT::i16 && MemVT != MVT::i32) ||
        Mask->getAPIntValue() !=
            APInt::getLowBitsSet(64, MemVT.getSizeInBits()))
      break;
    SDValue ZL = DAG.getExtLoad(ISD::ZEXTLOAD, SDLoc(LD), MVT::i64,
                                LD->getChain(), LD->getBasePtr(), MemVT,
                                LD->getMemOperand());
    // Other users of the anyext load are fine with the zero-extended value
    // (it is a refinement).  This also rewrites the FREEZE's operand.
    DAG.ReplaceAllUsesWith(LD, ZL.getNode());
    return DAG.getFreeze(ZL);
  }
  case KlaussCPUISD::SELECT_CC: {
    // A condition that became constant after lowering (e.g. bit 6 of an
    // i128 shift amount, known only once the *_PARTS node is expanded)
    // would otherwise still be a compare of two constants and a branch.
    SDValue TrueV = N->getOperand(3), FalseV = N->getOperand(4);
    if (TrueV == FalseV)
      return TrueV;
    auto CC = cast<CondCodeSDNode>(N->getOperand(2))->get();
    if (SDValue C = DAG.FoldSetCC(MVT::i64, N->getOperand(0), N->getOperand(1),
                                  CC, SDLoc(N));
        C && isa<ConstantSDNode>(C))
      return C->getAsZExtVal() ? TrueV : FalseV;
    break;
  }
  default:
    break;
  }
  return SDValue();
}

// The halves of a 128-bit shift by Shamt in [0, 128).  Bit 6 of Shamt picks
// the case.  The CPU only uses Shamt[5:0], so the shamt pattern turns the
// `Shamt - 64` of the >= 64 arm back into Shamt and `63 ^ Shamt` into ~Shamt.
// The DAG must still see an in-range amount in each arm: a shift by Shamt
// itself in the >= 64 arm is poison there, and once the combiner can prove
// Shamt >= 64 it folds that shift to 0 (KnownBits of an over-wide shift).
static SDValue getShiftPartsCond(SDValue Shamt, const SDLoc &DL,
                                 SelectionDAG &DAG) {
  EVT VT = Shamt.getValueType();
  SDValue Bit6 = DAG.getNode(ISD::AND, DL, VT, Shamt,
                             DAG.getConstant(64, DL, VT));
  return DAG.getSetCC(DL, VT, Bit6, DAG.getConstant(0, DL, VT), ISD::SETEQ);
}

SDValue KlaussCPUTargetLowering::LowerShiftLeftParts(SDValue Op,
                                                       SelectionDAG &DAG) const {
  SDLoc DL(Op);
  SDValue Lo = Op.getOperand(0);
  SDValue Hi = Op.getOperand(1);
  SDValue Shamt = Op.getOperand(2);
  EVT VT = MVT::i64;

  // if Shamt < 64:
  //   new_Lo = Lo << Shamt
  //   new_Hi = (Hi << Shamt) | ((Lo >> 1) >> (63 ^ Shamt))
  // else:
  //   new_Lo = 0
  //   new_Hi = Lo << (Shamt - 64)

  SDValue Zero       = DAG.getConstant(0, DL, VT);
  SDValue One        = DAG.getConstant(1, DL, VT);
  SDValue SixtyThree = DAG.getConstant(63, DL, VT);
  SDValue SixtyThreeMShamt = DAG.getNode(ISD::XOR, DL, VT, Shamt, SixtyThree);
  SDValue ShamtMinus64 =
      DAG.getNode(ISD::ADD, DL, VT, Shamt, DAG.getSignedConstant(-64, DL, VT));

  SDValue LoTrue       = DAG.getNode(ISD::SHL, DL, VT, Lo, Shamt);
  SDValue ShiftRight1Lo = DAG.getNode(ISD::SRL, DL, VT, Lo, One);
  SDValue ShiftRightLo  = DAG.getNode(ISD::SRL, DL, VT, ShiftRight1Lo, SixtyThreeMShamt);
  SDValue ShiftLeftHi   = DAG.getNode(ISD::SHL, DL, VT, Hi, Shamt);
  SDValue HiTrue  = DAG.getNode(ISD::OR,  DL, VT, ShiftLeftHi, ShiftRightLo);
  SDValue HiFalse = DAG.getNode(ISD::SHL, DL, VT, Lo, ShamtMinus64);

  SDValue CC = getShiftPartsCond(Shamt, DL, DAG);
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

  // if Shamt < 64:
  //   new_Lo = (Lo >> Shamt) | ((Hi << 1) << (63 ^ Shamt))
  //   new_Hi = Hi >> Shamt  (arithmetic for SRA, logical for SRL)
  // else:
  //   new_Lo = Hi >> (Shamt - 64)  (arithmetic for SRA)
  //   new_Hi = IsSRA ? Hi >> 63 : 0

  unsigned ShiftRightOp = IsSRA ? ISD::SRA : ISD::SRL;

  SDValue Zero       = DAG.getConstant(0, DL, VT);
  SDValue One        = DAG.getConstant(1, DL, VT);
  SDValue SixtyThree = DAG.getConstant(63, DL, VT);
  SDValue SixtyThreeMShamt = DAG.getNode(ISD::XOR, DL, VT, Shamt, SixtyThree);
  SDValue ShamtMinus64 =
      DAG.getNode(ISD::ADD, DL, VT, Shamt, DAG.getSignedConstant(-64, DL, VT));

  SDValue ShiftRightLo = DAG.getNode(ISD::SRL, DL, VT, Lo, Shamt);
  SDValue ShiftLeftHi1 = DAG.getNode(ISD::SHL, DL, VT, Hi, One);
  SDValue ShiftLeftHi  = DAG.getNode(ISD::SHL, DL, VT, ShiftLeftHi1, SixtyThreeMShamt);
  SDValue LoTrue  = DAG.getNode(ISD::OR,          DL, VT, ShiftRightLo, ShiftLeftHi);
  SDValue HiTrue  = DAG.getNode(ShiftRightOp,     DL, VT, Hi, Shamt);
  SDValue LoFalse = DAG.getNode(ShiftRightOp, DL, VT, Hi, ShamtMinus64);
  SDValue HiFalse = IsSRA ? DAG.getNode(ISD::SRA, DL, VT, Hi, SixtyThree) : Zero;

  SDValue CC = getShiftPartsCond(Shamt, DL, DAG);
  Lo = DAG.getNode(ISD::SELECT, DL, VT, CC, LoTrue, LoFalse);
  Hi = DAG.getNode(ISD::SELECT, DL, VT, CC, HiTrue, HiFalse);

  SDValue Parts[2] = {Lo, Hi};
  return DAG.getMergeValues(Parts, DL);
}

// Both SETRU and LEAPC build a 32-bit address zero-extended, so `sym+off`
// folded into the instruction is right only while S + off lies in
// [0, 2^32).  A negative offset can take it below zero (`&arr[i - K]` with
// arr at a low address and a large constant K): the folded form would yield
// 0x00000000_FFFFxxxx instead of the negative 64-bit value, and the 64-bit
// add of the index then carries into bit 32 (pointer compares and ptrtoint
// go wrong; the EA, bits [31:0], would still be right).  Fold only a
// non-negative offset below 2^31 (also the range of an ELFCLASS32 addend)
// and add any other one separately: SETRU sym + ADDI off is exact.
static SDValue wrapSymbolWithOffset(SelectionDAG &DAG, const SDLoc &DL,
                                    unsigned WrapOp, SDValue TSym,
                                    int64_t Offset) {
  SDValue Addr = DAG.getNode(WrapOp, DL, MVT::i64, TSym);
  if (Offset == 0)
    return Addr;
  return DAG.getNode(ISD::ADD, DL, MVT::i64, Addr,
                     DAG.getSignedConstant(Offset, DL, MVT::i64));
}

static bool isFoldableSymbolOffset(int64_t Offset) {
  return isUInt<31>(Offset);
}

SDValue KlaussCPUTargetLowering::LowerGlobalAddress(SDValue Op,
                                                      SelectionDAG &DAG) const {
  auto *N = cast<GlobalAddressSDNode>(Op);
  SDLoc DL(N);
  int64_t Offset = N->getOffset();
  bool Fold = isFoldableSymbolOffset(Offset);
  SDValue TGA = DAG.getTargetGlobalAddress(N->getGlobal(), DL, MVT::i64,
                                           Fold ? Offset : 0);
  // PIC: wrap in LEAPC → emits LEAPC with PCREL32 fixup.
  // Non-PIC: wrap in ADDR → emits SETRU with ABS32 fixup.
  unsigned WrapOp = getTargetMachine().isPositionIndependent()
                        ? KlaussCPUISD::LEAPC : KlaussCPUISD::ADDR;
  return wrapSymbolWithOffset(DAG, DL, WrapOp, TGA, Fold ? 0 : Offset);
}

SDValue KlaussCPUTargetLowering::LowerExternalSymbol(SDValue Op,
                                                       SelectionDAG &DAG) const {
  auto *N = cast<ExternalSymbolSDNode>(Op);
  SDValue TES = DAG.getTargetExternalSymbol(N->getSymbol(), MVT::i64);
  unsigned WrapOp = getTargetMachine().isPositionIndependent()
                        ? KlaussCPUISD::LEAPC : KlaussCPUISD::ADDR;
  return DAG.getNode(WrapOp, SDLoc(N), MVT::i64, TES);
}

SDValue KlaussCPUTargetLowering::LowerBlockAddress(SDValue Op,
                                                     SelectionDAG &DAG) const {
  auto *N = cast<BlockAddressSDNode>(Op);
  SDLoc DL(N);
  int64_t Offset = N->getOffset();
  bool Fold = isFoldableSymbolOffset(Offset);
  SDValue TBA = DAG.getTargetBlockAddress(N->getBlockAddress(), MVT::i64,
                                          Fold ? Offset : 0);
  unsigned WrapOp = getTargetMachine().isPositionIndependent()
                        ? KlaussCPUISD::LEAPC : KlaussCPUISD::ADDR;
  return wrapSymbolWithOffset(DAG, DL, WrapOp, TBA, Fold ? 0 : Offset);
}

SDValue KlaussCPUTargetLowering::LowerJumpTable(SDValue Op,
                                                  SelectionDAG &DAG) const {
  auto *N = cast<JumpTableSDNode>(Op);
  SDValue TJT = DAG.getTargetJumpTable(N->getIndex(), MVT::i64);
  unsigned WrapOp = getTargetMachine().isPositionIndependent()
                        ? KlaussCPUISD::LEAPC : KlaussCPUISD::ADDR;
  return DAG.getNode(WrapOp, SDLoc(N), MVT::i64, TJT);
}

// Jump tables use EK_Custom32: each 4-byte entry holds the absolute 32-bit
// target address.  The assembler emits R_KLAUSSCPU_ABS32 per entry; the
// linker fills them in regardless of which ELF section the table lands in.
// Dispatch: load 4-byte entry (absolute address), zero-extend, BRIND.
// The entry is used as-is (never added to the table base) — see
// isJumpTableRelative().  Under -fPIC only the table base is PC-relative.
SDValue KlaussCPUTargetLowering::LowerBR_JT(SDValue Op,
                                              SelectionDAG &DAG) const {
  SDValue Chain = Op.getOperand(0);
  SDValue Table = Op.getOperand(1);
  SDValue Index = Op.getOperand(2);
  SDLoc DL(Op);

  auto *JT = cast<JumpTableSDNode>(Table);
  unsigned WrapOp = getTargetMachine().isPositionIndependent()
                        ? KlaussCPUISD::LEAPC : KlaussCPUISD::ADDR;
  SDValue Base = DAG.getNode(WrapOp, DL, MVT::i64,
                             DAG.getTargetJumpTable(JT->getIndex(), MVT::i64));

  // EntryAddr = Base + Index * 4
  SDValue Scaled = DAG.getNode(ISD::SHL, DL, MVT::i64,
                                Index, DAG.getConstant(2, DL, MVT::i64));
  SDValue EntryAddr = DAG.getNode(ISD::ADD, DL, MVT::i64, Base, Scaled);

  // Load the 4-byte absolute target address from the table (zero-extend).
  // All code addresses fit in 32 bits, so zero-extension is correct.  This
  // selects the register-offset LDIDX32R, which force-aligns its EA; the
  // table is 4-byte aligned (EK_Custom32's entry alignment).
  MachineFunction &MF = DAG.getMachineFunction();
  SDValue Target =
      DAG.getExtLoad(ISD::ZEXTLOAD, DL, MVT::i64, Chain, EntryAddr,
                     MachinePointerInfo::getJumpTable(MF), MVT::i32);

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

bool KlaussCPUTargetLowering::isSuitableForJumpTable(
    const SwitchInst *SI, uint64_t NumCases, uint64_t Range,
    ProfileSummaryInfo *PSI, BlockFrequencyInfo *BFI) const {
  return NumCases * 100 >= Range * JumpTableDensity &&
         TargetLowering::isSuitableForJumpTable(SI, NumCases, Range, PSI, BFI);
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
  // A "cc"/"flags" clobber (clang adds "~{flags}" to every asm statement) makes
  // the INLINEASM implicit-def FLAGS, so no compare is scheduled across an asm
  // that may contain flag-setting instructions.
  if (Constraint.equals_insensitive("{cc}") ||
      Constraint.equals_insensitive("{flags}"))
    return {KlaussCPU::FLAGS, &KlaussCPU::CCRRegClass};
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
  // Chained so it is ordered against SETSP_R/ADDSP and never CSE'd with an
  // earlier read of a different SP value.
  SDNode *GetSP = DAG.getMachineNode(KlaussCPU::GETSP_R, DL, MVT::i64,
                                     MVT::Other, Chain);
  return DAG.getMergeValues({SDValue(GetSP, 0), SDValue(GetSP, 1)}, DL);
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

// Dynamic stack allocation (VLAs).
// LLVM's default expansion of DYNAMIC_STACKALLOC uses getCopyFromReg(SP) which
// asserts because SP has no register class.  Implement directly with GETSP_R /
// SETSP_R so we never touch SP as a physical-register operand.
// Stack grows downward; the frame pointer in R15 will restore SP on exit.
SDValue KlaussCPUTargetLowering::LowerDYNAMIC_STACKALLOC(SDValue Op,
                                                           SelectionDAG &DAG) const {
  SDLoc DL(Op);
  SDValue Chain  = Op.getOperand(0);
  SDValue Size   = Op.getOperand(1); // byte count (may be non-constant)
  SDValue AlignV = Op.getOperand(2); // alignment constant

  EVT VT = Op.getValueType(); // i64 (pointer type, result 0)

  // Read current SP via GETSP_R.  It MUST be chained: an unchained GETSP_R
  // has no operands, so SelectionDAG CSEs every one in the block into a single
  // node — two VLAs would then both be carved from the same original SP and
  // overlap.
  SDNode *GetSP = DAG.getMachineNode(KlaussCPU::GETSP_R, DL, MVT::i64,
                                     MVT::Other, Chain);
  SDValue OldSP(GetSP, 0);
  Chain = SDValue(GetSP, 1);

  // NewSP = OldSP - Size  (stack grows downward)
  SDValue NewSP = DAG.getNode(ISD::SUB, DL, VT, OldSP, Size);

  // Align NewSP DOWN to the requested alignment.
  if (auto *AC = dyn_cast<ConstantSDNode>(AlignV)) {
    uint64_t Align = AC->getZExtValue();
    if (Align > 1) {
      SDValue Mask = DAG.getConstant(~(Align - 1), DL, VT);
      NewSP = DAG.getNode(ISD::AND, DL, VT, NewSP, Mask);
    }
  }

  // Write new SP via SETSP_R.
  SDNode *SetSP = DAG.getMachineNode(KlaussCPU::SETSP_R, DL, MVT::Other,
                                      {NewSP, Chain});

  // Result: (allocated_address = NewSP, new_chain)
  return DAG.getMergeValues({NewSP, SDValue(SetSP, 0)}, DL);
}

// The core is in-order and single-issue, so its own loads and stores already
// take effect in program order (also as seen by its interrupt handlers): a
// fence only has to stop the compiler moving memory accesses across it (the
// __sync_* libcalls are calls, so nothing moves across them either).  It
// does NOT make stores visible to the second (AMP) core, whose view of DDR
// bypasses core 1's write-back D-cache; cross-core hand-offs need an explicit
// CACHE_CTRL FLUSH (MMIO_MAP.md) before the doorbell/flag write.
SDValue KlaussCPUTargetLowering::LowerATOMIC_FENCE(SDValue Op,
                                                     SelectionDAG &DAG) const {
  // MEMBARRIER is a compiler barrier; it codegens to a no-op.
  return DAG.getNode(ISD::MEMBARRIER, SDLoc(Op), MVT::Other,
                     Op.getOperand(0));
}

// Every function has a frame (hasFP is always true): R15 points at the saved
// caller R15, and the return address is the slot above it, so
//   frameaddress(0)  = R15
//   frameaddress(N)  = [frameaddress(N-1)]
//   returnaddress(N) = [frameaddress(N) + 8]
SDValue KlaussCPUTargetLowering::LowerFRAMEADDR(SDValue Op,
                                                  SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  MF.getFrameInfo().setFrameAddressIsTaken(true);
  EVT VT = Op.getValueType();
  SDLoc DL(Op);
  SDValue FrameAddr =
      DAG.getCopyFromReg(DAG.getEntryNode(), DL, KlaussCPU::R15, VT);
  unsigned Depth = Op.getConstantOperandVal(0);
  while (Depth--)
    FrameAddr = DAG.getLoad(VT, DL, DAG.getEntryNode(), FrameAddr,
                            MachinePointerInfo());
  return FrameAddr;
}

SDValue KlaussCPUTargetLowering::LowerRETURNADDR(SDValue Op,
                                                   SelectionDAG &DAG) const {
  DAG.getMachineFunction().getFrameInfo().setReturnAddressIsTaken(true);
  EVT VT = Op.getValueType();
  SDLoc DL(Op);
  SDValue FrameAddr = LowerFRAMEADDR(Op, DAG);
  SDValue Ptr = DAG.getNode(ISD::ADD, DL, VT, FrameAddr,
                            DAG.getConstant(8, DL, VT));
  return DAG.getLoad(VT, DL, DAG.getEntryNode(), Ptr, MachinePointerInfo());
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

// KlaussCPU va_arg lowering.
//
// The calling convention promotes every sub-word argument type (i8/i16/i32)
// to i64, so every va_list slot is 8 bytes regardless of the C type.  The
// generic VAARG expansion would read only sizeof(type) bytes for an i32
// va_arg, leaving the upper 32 bits of the destination register undefined.
// This custom lowering always reads a full 8-byte slot, then truncates the
// result to the requested type — matching what the caller stored when it
// promoted the argument.
SDValue KlaussCPUTargetLowering::LowerVAARG(SDValue Op,
                                              SelectionDAG &DAG) const {
  SDNode *Node  = Op.getNode();
  EVT VT        = Node->getValueType(0);
  SDValue Chain = Node->getOperand(0);
  SDValue VAPtr = Node->getOperand(1);  // pointer to the va_list object
  const Value *SV = cast<SrcValueSDNode>(Node->getOperand(2))->getValue();
  SDLoc DL(Node);

  // Step 1: load the current argument pointer stored inside va_list.
  SDValue APRaw = DAG.getLoad(MVT::i64, DL, Chain, VAPtr, MachinePointerInfo(SV));
  Chain = APRaw.getValue(1);

  // Step 2: align to 8 (all slots are i64-sized): ptr = (ptr + 7) & -8
  SDValue AP = DAG.getNode(
      ISD::AND, DL, MVT::i64,
      DAG.getNode(ISD::ADD, DL, MVT::i64, APRaw,
                  DAG.getConstant(7, DL, MVT::i64)),
      DAG.getConstant(-8LL, DL, MVT::i64));

  // Step 3: advance the va_list pointer by 8 and store it back.
  SDValue Next = DAG.getNode(ISD::ADD, DL, MVT::i64, AP,
                              DAG.getConstant(8, DL, MVT::i64));
  Chain = DAG.getStore(Chain, DL, Next, VAPtr, MachinePointerInfo(SV));

  // Step 4: load the full 8-byte slot.
  SDValue Val = DAG.getLoad(MVT::i64, DL, Chain, AP, MachinePointerInfo());
  Chain = Val.getValue(1);

  // Step 5: truncate to the requested type if narrower than i64.
  if (VT != MVT::i64)
    Val = DAG.getNode(ISD::TRUNCATE, DL, VT, Val);

  return DAG.getMergeValues({Val, Chain}, DL);
}

const char *
KlaussCPUTargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch (Opcode) {
  case KlaussCPUISD::RET_GLUE: return "KlaussCPUISD::RET_GLUE";
  case KlaussCPUISD::CALL:     return "KlaussCPUISD::CALL";
  case KlaussCPUISD::ADDR:     return "KlaussCPUISD::ADDR";
  case KlaussCPUISD::LEAPC:    return "KlaussCPUISD::LEAPC";
  case KlaussCPUISD::SELECT_CC: return "KlaussCPUISD::SELECT_CC";
  case KlaussCPUISD::ROLW:     return "KlaussCPUISD::ROLW";
  case KlaussCPUISD::RORW:     return "KlaussCPUISD::RORW";
  default: return nullptr;
  }
}

//===----------------------------------------------------------------------===//
// Formal arguments — callee side
//
// Stack layout (from the callee's incoming SP, which CALL has decremented
// by 8 to push the return address):
//   [SP+0..SP+7]   return address
//   [SP+8..SP+31]  reserved: home area for R1-R3 (varargs)
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
      // getFrameIndexReference adds 8 for fixed objects
      // (R15 = incoming_SP - 8).
      int FI = MF.getFrameInfo().CreateFixedObject(8, VA.getLocMemOffset(),
                                                    /*isImmutable=*/true);
      SDValue FIN = DAG.getFrameIndex(FI, getPointerTy(DAG.getDataLayout()));
      InVals.push_back(DAG.getLoad(MVT::i64, DL, Chain, FIN,
                                    MachinePointerInfo::getFixedStack(MF, FI)));
    }
  }

  if (IsVarArg) {
    static const MCPhysReg ArgRegs[] = {
        KlaussCPU::R0, KlaussCPU::R1, KlaussCPU::R2, KlaussCPU::R3};
    constexpr unsigned NumArgRegs = 4;

    // Index of the first unallocated (= first potential vararg) register.
    unsigned FirstVaReg =
        CCInfo.getFirstUnallocated(ArrayRef<MCPhysReg>(ArgRegs, NumArgRegs));

    // The caller reserves home slots for R1–R3 only: R0's would be
    // [incoming_SP+0], the return address.  So a function with no named
    // parameter (C23 `f(...)`, or IR) cannot spill R0 next to the others.
    if (FirstVaReg == 0) {
      DAG.getContext()->diagnose(DiagnosticInfoUnsupported(
          MF.getFunction(),
          "variadic functions without a named parameter are not supported "
          "by the KlaussCPU calling convention",
          DL.getDebugLoc()));
      FirstVaReg = 1;
    }

    auto &MFI = MF.getFrameInfo();

    // Arg register R_I sits in the caller's "reserved area" at [incoming_SP + I*8].
    // We save R_{FirstVaReg}..R3 there so they are contiguous with stack args at
    // [incoming_SP+32..], allowing va_arg to advance a plain pointer through all args.
    //
    // getFrameIndexReference adds 8 for fixed objects (R15 = incoming_SP - 8),
    // so CreateFixedObject with offset F yields the R15-relative offset F+8.

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

// A return value that does not fit in R12/R11 is demoted to an sret pointer
// argument by SelectionDAG.
bool KlaussCPUTargetLowering::CanLowerReturn(
    CallingConv::ID CallConv, MachineFunction &MF, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs, LLVMContext &Context,
    const Type *RetTy) const {
  SmallVector<CCValAssign, 4> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, RVLocs, Context);
  return CCInfo.CheckReturn(Outs, RetCC_KlaussCPU);
}

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
// CALL pushes the return address (SP -= 8), so the callee's incoming SP is
// the caller's SP - 8.  Callee sees: [SP+0..7] = return address,
// [SP+8..31] = reserved, [SP+32..] = stack args; the caller therefore stores
// stack args from its SP + 24 and pre-reserves 24 bytes in CCState.
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
  // Pointers are 64-bit (DataLayout p:64:64), so GPRs hold them natively.
  MVT PtrVT  = getPointerTy(DAG.getDataLayout()); // MVT::i64
  MVT CallVT = MVT::i64;                          // GPR-width callee operand

  // ---- Analyze outgoing arguments ----------------------------------------
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, ArgLocs, *DAG.getContext());
  // CALL pre-decrements SP by 8 (stores return address at [SP-8], SP -= 8).
  // So incoming_SP = CallerSP - 8.  From the caller's GETSP_R perspective:
  //   [GETSP-8..-1]  = return address slot  (incoming_SP +0..+7)
  //   [GETSP+0..23]  = reserved area for R1-R3 saves (incoming_SP +8..+31)
  //   [GETSP+24..]   = stack args (incoming_SP +32 onwards)
  // The callee pre-reserves 32 bytes from incoming_SP.  The caller's GETSP
  // value is incoming_SP + 8, so pre-reserve only 24 bytes here so the first
  // stack arg offset (24) maps to incoming_SP + 32 on the callee side.
  CCInfo.AllocateStack(24, Align(8));
  CCInfo.AnalyzeCallOperands(CLI.Outs, CC_KlaussCPU);

  unsigned StackSize = CCInfo.getStackSize(); // ≥ 24

  // ---- Make private copies of byval arguments ----------------------------
  // A byval argument is passed as a pointer (CC_KlaussCPU treats it as an
  // ordinary i64), but the callee owns the pointee and may modify it, so the
  // caller must pass the address of a fresh copy.  The copies are made before
  // CALLSEQ_START: a large copy becomes a memcpy call, which must not nest
  // inside this call's sequence or clobber its outgoing stack arguments.
  SmallVector<SDValue, 4> ByValArgs;
  for (unsigned I = 0, E = CLI.Outs.size(); I != E; ++I) {
    ISD::ArgFlagsTy Flags = CLI.Outs[I].Flags;
    if (!Flags.isByVal())
      continue;

    // Clang's DefaultABIInfo passes empty records (GNU C `struct {}`, or a
    // struct holding only a zero-length array) byval.  The callee can read
    // no bytes, so pass the original pointer; a zero-size stack object
    // is not allowed.
    unsigned Size = Flags.getByValSize();
    if (Size == 0) {
      ByValArgs.push_back(CLI.OutVals[I]);
      continue;
    }

    Align Alignment = Flags.getNonZeroByValAlign();
    int FI = MF.getFrameInfo().CreateStackObject(Size, Alignment,
                                                 /*isSpillSlot=*/false);
    SDValue FIPtr = DAG.getFrameIndex(FI, PtrVT);
    Chain = DAG.getMemcpy(Chain, DL, FIPtr, CLI.OutVals[I],
                          DAG.getConstant(Size, DL, PtrVT), Alignment,
                          /*isVol=*/false, /*AlwaysInline=*/false,
                          /*CI=*/nullptr, /*OverrideTailCall=*/false,
                          MachinePointerInfo::getFixedStack(MF, FI),
                          MachinePointerInfo());
    ByValArgs.push_back(FIPtr);
  }

  Chain = DAG.getCALLSEQ_START(Chain, StackSize, 0, DL);

  // ---- Collect register and memory args ----------------------------------
  SmallVector<std::pair<Register, SDValue>, 8> RegsToPass;
  SmallVector<SDValue, 4> MemOpChains;

  // Use GETSP_R to read the 32-bit SP into an i64 GPR (zero-extends).
  // Avoid getCopyFromReg(SP, i32): the type legalizer cannot promote
  // i32 CopyFromReg nodes for physical-only registers (no i32 reg class).
  // Chained after CALLSEQ_START: when a VLA disables the reserved call frame,
  // ADJCALLSTACKDOWN becomes an ADDSP and the stack args must be stored
  // relative to the SP *after* it (an unchained GETSP_R was CSE'd with the
  // function's first SP read and stored the args above the VLA).
  // Only emitted when a stack argument needs it: a chained side-effecting
  // node is never dead-code eliminated.
  SDValue SP;
  if (llvm::any_of(ArgLocs, [](const CCValAssign &VA) { return VA.isMemLoc(); })) {
    SDNode *GetSP = DAG.getMachineNode(KlaussCPU::GETSP_R, DL, PtrVT,
                                       MVT::Other, Chain);
    SP = SDValue(GetSP, 0);
    Chain = SDValue(GetSP, 1);
  }

  for (unsigned I = 0, J = 0, E = ArgLocs.size(); I != E; ++I) {
    const CCValAssign &VA = ArgLocs[I];
    SDValue Arg = CLI.OutVals[I];
    // Pass the address of the caller-side copy instead of the original.
    if (CLI.Outs[I].Flags.isByVal())
      Arg = ByValArgs[J++];

    if (VA.isRegLoc()) {
      RegsToPass.push_back({VA.getLocReg(), Arg});
    } else {
      assert(VA.isMemLoc());
      // Offset already includes the 24-byte pre-reservation.
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

  // ---- Copy return values from R12/R11 ------------------------------------
  // LowerCallTo only splits i128 into {i64, i64} in CLI.Ins when
  // IsPostTypeLegalization=true.  When makeLibCall is called from the type
  // legalizer (e.g. ExpandIntRes_UDIV for __udivti3), that flag is false, so
  // CLI.Ins contains one raw i128 entry.  RetCC_KlaussCPU has no i128 rule
  // and AnalyzeCallResult would hit llvm_unreachable.
  // Fix: split every i128 entry into two i64 entries here, so the CC sees
  // what it expects.  LowerCallTo's getCopyFromParts() then reassembles the
  // i128 from the two i64 values in InVals.
  SmallVector<ISD::InputArg, 8> AdjIns;
  for (const auto &In : CLI.Ins) {
    if (In.VT == MVT::i128) {
      ISD::InputArg Half = In;
      Half.VT = MVT::i64;
      AdjIns.push_back(Half); // Lo → R12
      AdjIns.push_back(Half); // Hi → R11
    } else {
      AdjIns.push_back(In);
    }
  }

  SmallVector<CCValAssign, 4> RVLocs;
  CCState RetCCInfo(CallConv, IsVarArg, MF, RVLocs, *DAG.getContext());
  RetCCInfo.AnalyzeCallResult(AdjIns, RetCC_KlaussCPU);

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

// SELECT / SELECT_CC: no hardware CMOV.  Both become KlaussCPUISD::SELECT_CC,
// which is selected to SELECT_CC_RR/RI and expanded to branches by
// EmitInstrWithCustomInserter below.
SDValue KlaussCPUTargetLowering::buildSelectCC(const SDLoc &DL, SDValue LHS,
                                               SDValue RHS, ISD::CondCode CC,
                                               SDValue TrueV, SDValue FalseV,
                                               SelectionDAG &DAG) const {
  EVT VT = TrueV.getValueType();
  if (LHS.getValueType() != MVT::i64 ||
      !KlaussCPUInstrInfo::getCondBranchOpcode(CC, /*PIC=*/false)) {
    // No direct jump for this compare: materialise it, then test != 0.
    LHS = DAG.getSetCC(DL, MVT::i64, LHS, RHS, CC);
    RHS = DAG.getConstant(0, DL, MVT::i64);
    CC = ISD::SETNE;
  }
  return DAG.getNode(KlaussCPUISD::SELECT_CC, DL, VT, LHS, RHS,
                     DAG.getCondCode(CC), TrueV, FalseV);
}

SDValue KlaussCPUTargetLowering::LowerSELECT(SDValue Op,
                                             SelectionDAG &DAG) const {
  SDLoc DL(Op);
  SDValue Cond = Op.getOperand(0);
  // select (setcc a, b, cc), t, f  →  SELECT_CC a, b, cc, t, f
  if (Cond.getOpcode() == ISD::SETCC &&
      Cond.getOperand(0).getValueType() == MVT::i64)
    return buildSelectCC(DL, Cond.getOperand(0), Cond.getOperand(1),
                         cast<CondCodeSDNode>(Cond.getOperand(2))->get(),
                         Op.getOperand(1), Op.getOperand(2), DAG);
  return buildSelectCC(DL, Cond, DAG.getConstant(0, DL, MVT::i64),
                       ISD::SETNE, Op.getOperand(1), Op.getOperand(2), DAG);
}

SDValue KlaussCPUTargetLowering::LowerSELECT_CC(SDValue Op,
                                                SelectionDAG &DAG) const {
  // ISD::SELECT_CC operands: (lhs, rhs, trueV, falseV, cc)
  return buildSelectCC(SDLoc(Op), Op.getOperand(0), Op.getOperand(1),
                       cast<CondCodeSDNode>(Op.getOperand(4))->get(),
                       Op.getOperand(2), Op.getOperand(3), DAG);
}

// Does Next test the same condition as the select pseudo MI: same opcode,
// operands and $jmp (which carries the W-compare flag)?
static bool isSameSelectCondition(const MachineInstr &MI,
                                  const MachineInstr &Next) {
  if (Next.getOpcode() != MI.getOpcode() ||
      Next.getOperand(1).getReg() != MI.getOperand(1).getReg() ||
      Next.getOperand(3).getImm() != MI.getOperand(3).getImm())
    return false;
  const MachineOperand &RHS = MI.getOperand(2), &NextRHS = Next.getOperand(2);
  return RHS.isReg() ? NextRHS.getReg() == RHS.getReg()
                     : NextRHS.getImm() == RHS.getImm();
}

// Expand SELECT_CC_RR/RI to a diamond with a PHI at the sink:
//
//   TestBB:  CMPRR lhs, rhs   (CMPRV lhs, imm for _RI)
//            JMPcc SinkBB     ← condition true → trueV
//            (fall through to FalseBB)
//   FalseBB: (empty, falls through to SinkBB)
//   SinkBB:  DstReg = PHI [ TrueReg, TestBB ], [ FalseReg, FalseBB ]
//            ... (rest of original BB) ...
//
// This preserves SSA: DstReg has exactly one definition (the PHI).
// TrueReg and FalseReg are defined in the original BB which dominates all paths.
//
// As in RISCVTargetLowering's emitSelectPseudo, a run of select pseudos on
// the same condition shares the compare and branch and gets one PHI each
// (a swap `c ? x : y; c ? y : x`, the two selects of a 128-bit shift, the
// soft-float operand ordering).  The run may contain other instructions;
// they stay in TestBB, ahead of the compare, so the run ends at one that
// could not be moved ahead of the selects: one with side effects, a memory
// access, a call-frame pseudo, a physical register def other than FLAGS or
// a read of FLAGS, or one that uses a select's result.  A select whose true
// or false value is the result of an earlier select in the run also ends it
// (its PHI would read a value not defined in TestBB).  Debug instructions
// after the first select move to SinkBB, behind the PHIs, in their order.
MachineBasicBlock *KlaussCPUTargetLowering::EmitInstrWithCustomInserter(
    MachineInstr &MI, MachineBasicBlock *BB) const {
  unsigned Opc = MI.getOpcode();
  assert((Opc == KlaussCPU::SELECT_CC_RR || Opc == KlaussCPU::SELECT_CC_RI) &&
         "unexpected custom-inserter instruction");

  const KlaussCPUInstrInfo &TII =
      *BB->getParent()->getSubtarget<KlaussCPUSubtarget>().getInstrInfo();
  DebugLoc DL = MI.getDebugLoc();
  MachineFunction *MF = BB->getParent();
  const BasicBlock *LLVMBB = BB->getBasicBlock();

  Register LHSReg   = MI.getOperand(1).getReg();
  const MachineOperand &RHS = MI.getOperand(2);
  unsigned JmpOpc   = MI.getOperand(3).getImm();
  // ISA v3 D2: flag bit → 32-bit W compare (see KlaussCPUDAGToDAGISel).
  bool W32 = JmpOpc & KlaussCPU::SelectCCW32Flag;
  JmpOpc &= ~KlaussCPU::SelectCCW32Flag;

  // Collect the run of selects on MI's condition.
  SmallSet<Register, 4> SelectDests;
  SelectDests.insert(MI.getOperand(0).getReg());
  MachineInstr *LastSelect = &MI;
  if (GroupSelects) {
    for (MachineInstr &I :
         make_range(std::next(MachineBasicBlock::iterator(MI)), BB->end())) {
      if (I.isDebugInstr())
        continue;
      if (I.getOpcode() == KlaussCPU::SELECT_CC_RR ||
          I.getOpcode() == KlaussCPU::SELECT_CC_RI) {
        if (!isSameSelectCondition(MI, I) ||
            SelectDests.count(I.getOperand(4).getReg()) ||
            SelectDests.count(I.getOperand(5).getReg()))
          break;
        SelectDests.insert(I.getOperand(0).getReg());
        LastSelect = &I;
        continue;
      }
      if (I.isTerminator() || I.isPHI() || I.hasUnmodeledSideEffects() ||
          I.mayLoadOrStore() || I.usesCustomInsertionHook() ||
          TII.isFrameInstr(I) || I.readsRegister(KlaussCPU::FLAGS, nullptr))
        break;
      if (any_of(I.operands(), [&](const MachineOperand &MO) {
            if (!MO.isReg() || !MO.getReg())
              return false;
            if (MO.isUse())
              return SelectDests.count(MO.getReg()) != 0;
            return MO.getReg().isPhysical() && MO.getReg() != KlaussCPU::FLAGS;
          }))
        break;
    }
  }

  MachineBasicBlock *FalseBB = MF->CreateMachineBasicBlock(LLVMBB);
  MachineBasicBlock *SinkBB  = MF->CreateMachineBasicBlock(LLVMBB);

  MachineFunction::iterator It = ++BB->getIterator();
  MF->insert(It, FalseBB);
  MF->insert(It, SinkBB);

  // The select may sit inside a call sequence (a call argument); the new
  // blocks start with the same call frame set up.
  unsigned CallFrameSize = TII.getCallFrameSizeAt(*LastSelect);
  FalseBB->setCallFrameSize(CallFrameSize);
  SinkBB->setCallFrameSize(CallFrameSize);

  // Pull the selects (into PHIs) and the debug instructions out of the run;
  // the other instructions stay in BB.
  SmallVector<MachineInstr *, 4> Selects, DebugInstrs;
  auto RunEnd = std::next(MachineBasicBlock::iterator(LastSelect));
  for (MachineInstr &I : make_range(MachineBasicBlock::iterator(MI), RunEnd)) {
    if (I.getOpcode() == KlaussCPU::SELECT_CC_RR ||
        I.getOpcode() == KlaussCPU::SELECT_CC_RI)
      Selects.push_back(&I);
    else if (I.isDebugInstr())
      DebugInstrs.push_back(&I);
  }
  for (MachineInstr *DI : DebugInstrs)
    SinkBB->push_back(DI->removeFromParent());

  // Move the tail of BB (after the run) into SinkBB.
  SinkBB->splice(SinkBB->end(), BB, RunEnd, BB->end());
  SinkBB->transferSuccessorsAndUpdatePHIs(BB);

  // TestBB: compare, and jump to SinkBB (true value) when the condition holds.
  if (Opc == KlaussCPU::SELECT_CC_RR)
    BuildMI(BB, DL, TII.get(W32 ? KlaussCPU::CMPRRW_I : KlaussCPU::CMPRR_I))
        .addReg(LHSReg)
        .addReg(RHS.getReg());
  else if (W32 || isInt<32>(RHS.getImm()))
    BuildMI(BB, DL, TII.get(W32 ? KlaussCPU::CMPRVW_I : KlaussCPU::CMPRV_I))
        .addReg(LHSReg)
        .addImm(RHS.getImm());
  else // [2^31, 2^32): zero-extended imm32
    BuildMI(BB, DL, TII.get(KlaussCPU::CMPRVU_I))
        .addReg(LHSReg)
        .addImm(RHS.getImm());
  BuildMI(BB, DL, TII.get(JmpOpc)).addMBB(SinkBB);
  // addSuccessorWithoutProb puts the blocks in "no probability tracking" mode.
  // Explicit probabilities would later clash with passes (e.g. BranchFolder)
  // that call addSuccessor(Unknown) on the same block, creating a
  // Probs.size() != Successors.size() mismatch.
  BB->addSuccessorWithoutProb(SinkBB);  // condition true (jump taken)
  BB->addSuccessorWithoutProb(FalseBB); // condition false (fall through)

  // FalseBB: empty, just falls through to SinkBB.
  FalseBB->addSuccessorWithoutProb(SinkBB);

  // SinkBB: one PHI per select merges its two values; each Dst has one def.
  auto InsertPt = SinkBB->begin();
  for (MachineInstr *Sel : Selects) {
    BuildMI(*SinkBB, InsertPt, Sel->getDebugLoc(), TII.get(TargetOpcode::PHI),
            Sel->getOperand(0).getReg())
        .addReg(Sel->getOperand(4).getReg()).addMBB(BB)
        .addReg(Sel->getOperand(5).getReg()).addMBB(FalseBB);
    Sel->eraseFromParent();
  }
  return SinkBB;
}
