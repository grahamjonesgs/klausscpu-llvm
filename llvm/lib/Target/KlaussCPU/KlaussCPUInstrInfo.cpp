//===-- KlaussCPUInstrInfo.cpp - KlaussCPU Instruction Information --------===//
//
// KlaussCPU LLVM backend — instruction info implementation.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUInstrInfo.h"
#include "KlaussCPURegisterInfo.h"
#include "KlaussCPUSubtarget.h"
#include "MCTargetDesc/KlaussCPUMCTargetDesc.h"
#include "llvm/CodeGen/CFIInstBuilder.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/ScheduleDAGInstrs.h"
#include "llvm/CodeGen/ScheduleHazardRecognizer.h"
#include "llvm/CodeGen/TargetFrameLowering.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/TargetParser/KlaussCPUABI.h"

#define GET_INSTRINFO_CTOR_DTOR
#include "KlaussCPUGenInstrInfo.inc"

using namespace llvm;

#define GEN_CHECK_COMPRESS_INSTR
#include "KlaussCPUGenCompressInstEmitter.inc"

static cl::opt<bool> PostRASingleIssue(
    "klausscpu-postra-single-issue", cl::Hidden, cl::init(true),
    cl::desc("Post-RA list scheduler issues one instruction per cycle.  "
             "=false restores LLVM's default (unlimited issue) for A/B."));

static cl::opt<bool> PostRAExBusy(
    "klausscpu-postra-ex-busy", cl::Hidden, cl::init(true),
    cl::desc("Post-RA list scheduler keeps the issue slot busy for a "
             "multi-cycle op's ReleaseAtCycles (MUL 4, DIV 20), as the RTL's "
             "EX hold does.  =false: every op takes one slot (A/B)."));

KlaussCPUInstrInfo::KlaussCPUInstrInfo(const KlaussCPUSubtarget &STI_,
                                        const KlaussCPURegisterInfo &RI)
    : KlaussCPUGenInstrInfo(STI_, RI,
                            KlaussCPU::ADJCALLSTACKDOWN,
                            KlaussCPU::ADJCALLSTACKUP),
      STI(STI_) {}

void KlaussCPUInstrInfo::storeRegToStackSlot(MachineBasicBlock &MBB,
                                              MachineBasicBlock::iterator I,
                                              Register SrcReg, bool IsKill,
                                              int FI,
                                              const TargetRegisterClass *RC,
                                              Register VReg,
                                              MachineInstr::MIFlag Flags) const {
  assert(KlaussCPU::GPRRegClass.hasSubClassEq(RC) &&
         "KlaussCPU storeRegToStackSlot: only GPR spills supported");
  MachineFunction &MF = *MBB.getParent();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  MachineMemOperand *MMO = MF.getMachineMemOperand(
      MachinePointerInfo::getFixedStack(MF, FI), MachineMemOperand::MOStore,
      MFI.getObjectSize(FI), MFI.getObjectAlign(FI));
  // STIDX64 data, base, offset — base/offset filled in by eliminateFrameIndex.
  BuildMI(MBB, I, DebugLoc(), get(KlaussCPU::STIDX64))
      .addReg(SrcReg, getKillRegState(IsKill))
      .addFrameIndex(FI)
      .addImm(0)
      .addMemOperand(MMO)
      .setMIFlag(Flags);
}

void KlaussCPUInstrInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
                                               MachineBasicBlock::iterator I,
                                               Register DstReg, int FI,
                                               const TargetRegisterClass *RC,
                                               Register VReg,
                                               unsigned SubReg,
                                               MachineInstr::MIFlag Flags) const {
  assert(KlaussCPU::GPRRegClass.hasSubClassEq(RC) &&
         "KlaussCPU loadRegFromStackSlot: only GPR spills supported");
  MachineFunction &MF = *MBB.getParent();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  MachineMemOperand *MMO = MF.getMachineMemOperand(
      MachinePointerInfo::getFixedStack(MF, FI), MachineMemOperand::MOLoad,
      MFI.getObjectSize(FI), MFI.getObjectAlign(FI));
  // LDIDX64 dst, base, offset — base/offset filled in by eliminateFrameIndex.
  BuildMI(MBB, I, DebugLoc(), get(KlaussCPU::LDIDX64), DstReg)
      .addFrameIndex(FI)
      .addImm(0)
      .addMemOperand(MMO)
      .setMIFlag(Flags);
}

bool KlaussCPUInstrInfo::flagsDeadAfter(const MachineInstr &Br) {
  const MachineBasicBlock &MBB = *Br.getParent();
  // Without block live-ins the successor check below proves nothing.
  if (!MBB.getParent()->getRegInfo().tracksLiveness())
    return false;
  for (auto I = std::next(Br.getIterator()); I != MBB.end(); ++I) {
    if (I->readsRegister(KlaussCPU::FLAGS, /*TRI=*/nullptr))
      return false;
    if (I->modifiesRegister(KlaussCPU::FLAGS, /*TRI=*/nullptr))
      return true;
  }
  for (const MachineBasicBlock *S : MBB.successors())
    if (S->isLiveIn(KlaussCPU::FLAGS))
      return false;
  return true;
}

unsigned KlaussCPUInstrInfo::getCondBranchOpcode(ISD::CondCode CC, bool PIC) {
  // Post flag-unification (RTL fbb77d7 / FLAG_UNIFICATION_CHANGES) there is a
  // single Z/S/C/V flags register: CMP is SUB-without-writeback and each COND
  // is derived in hardware (EQ=Z, signed LT=S^V, unsigned ULT=C, ...).
  // BORROW POLARITY (x86, NOT ARM): after SUB/CMP, unsigned `<` is C and
  // unsigned `>=` is ¬C.  SETULT→JMPULT / SETUGE→JMPUGE encode this; do NOT
  // invert it in any future carry-based (ADC/SBC / setcc-from-carry) lowering.
  switch (CC) {
  case ISD::SETEQ:  return PIC ? KlaussCPU::JMPEREL   : KlaussCPU::JMPE;
  case ISD::SETNE:  return PIC ? KlaussCPU::JMPNEREL  : KlaussCPU::JMPNE;
  case ISD::SETLT:  return PIC ? KlaussCPU::JMPLTREL  : KlaussCPU::JMPLT;
  case ISD::SETLE:  return PIC ? KlaussCPU::JMPLEREL  : KlaussCPU::JMPLE;
  case ISD::SETGT:  return PIC ? KlaussCPU::JMPGTREL  : KlaussCPU::JMPGT;
  case ISD::SETGE:  return PIC ? KlaussCPU::JMPGEREL  : KlaussCPU::JMPGE;
  case ISD::SETULT: return PIC ? KlaussCPU::JMPULTREL : KlaussCPU::JMPULT;
  case ISD::SETULE: return PIC ? KlaussCPU::JMPULEREL : KlaussCPU::JMPULE;
  case ISD::SETUGT: return PIC ? KlaussCPU::JMPUGTREL : KlaussCPU::JMPUGT;
  case ISD::SETUGE: return PIC ? KlaussCPU::JMPUGEREL : KlaussCPU::JMPUGE;
  default:          return 0;
  }
}

// Conditional-branch opcode pairs: {branch, inverse}.  Covers the absolute
// and PC-relative (PIC) forms.  Used by analyzeBranch (to recognise a
// conditional branch) and reverseBranchCondition.
static const unsigned CondBranchInverse[][2] = {
    {KlaussCPU::JMPZ, KlaussCPU::JMPNZ},     {KlaussCPU::JMPE, KlaussCPU::JMPNE},
    {KlaussCPU::JMPC, KlaussCPU::JMPNC},     {KlaussCPU::JMPS, KlaussCPU::JMPNS},
    {KlaussCPU::JMPO, KlaussCPU::JMPNO},     {KlaussCPU::JMPLT, KlaussCPU::JMPGE},
    {KlaussCPU::JMPLE, KlaussCPU::JMPGT},    {KlaussCPU::JMPULT, KlaussCPU::JMPUGE},
    {KlaussCPU::JMPULE, KlaussCPU::JMPUGT},
    {KlaussCPU::JMPZREL, KlaussCPU::JMPNZREL},
    {KlaussCPU::JMPEREL, KlaussCPU::JMPNEREL},
    {KlaussCPU::JMPCREL, KlaussCPU::JMPNCREL},
    {KlaussCPU::JMPSREL, KlaussCPU::JMPNSREL},
    {KlaussCPU::JMPLTREL, KlaussCPU::JMPGEREL},
    {KlaussCPU::JMPLEREL, KlaussCPU::JMPGTREL},
    {KlaussCPU::JMPULTREL, KlaussCPU::JMPUGEREL},
    {KlaussCPU::JMPULEREL, KlaussCPU::JMPUGTREL},
};

// Inverse of a conditional branch opcode, or 0 if Opc is not one.
static unsigned getInverseCondBranch(unsigned Opc) {
  for (const auto &P : CondBranchInverse) {
    if (P[0] == Opc)
      return P[1];
    if (P[1] == Opc)
      return P[0];
  }
  return 0;
}

// A direct (immediate-target) branch: JMP/JMPREL or any conditional JMPxx.
// Indirect JMPR_R and returns are terminators but not direct branches.
static bool isDirectBranch(const MachineInstr &MI) {
  return MI.getDesc().isBranch() && !MI.getDesc().isIndirectBranch();
}

bool KlaussCPUInstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                        MachineBasicBlock *&TBB,
                                        MachineBasicBlock *&FBB,
                                        SmallVectorImpl<MachineOperand> &Cond,
                                        bool AllowModify) const {
  // Cond encoding: a single immediate holding the conditional JMPxx opcode.
  // The flag-setting compare stays in the block — removeBranch/insertBranch
  // only touch the jumps, so the compare/branch pairing survives rewrites.
  // (Modelled on RISCVInstrInfo::analyzeBranch.)
  TBB = FBB = nullptr;
  Cond.clear();

  MachineBasicBlock::iterator I = MBB.getLastNonDebugInstr();
  if (I == MBB.end() || !isUnpredicatedTerminator(*I))
    return false; // falls through

  // Count terminators; find the first unconditional or indirect branch.
  MachineBasicBlock::iterator FirstUncondOrIndirect = MBB.end();
  int NumTerminators = 0;
  for (auto J = I.getReverse(); J != MBB.rend() && isUnpredicatedTerminator(*J);
       ++J) {
    // ISA v3 B fused branches (pre-emit only) carry their target at
    // operand 3 and no separate compare: not modelled here.
    if (J->getOpcode() == KlaussCPU::FBR_RR || J->getOpcode() == KlaussCPU::FBR_RI)
      return true;
    ++NumTerminators;
    if (J->getDesc().isUnconditionalBranch() ||
        J->getDesc().isIndirectBranch())
      FirstUncondOrIndirect = J.getReverse();
  }

  // Anything after an unconditional/indirect branch is dead.
  if (AllowModify && FirstUncondOrIndirect != MBB.end()) {
    while (std::next(FirstUncondOrIndirect) != MBB.end()) {
      std::next(FirstUncondOrIndirect)->eraseFromParent();
      --NumTerminators;
    }
    I = FirstUncondOrIndirect;
  }

  // Returns, indirect branches (JMPR_R) and anything else we don't model.
  if (!isDirectBranch(*I) || NumTerminators > 2)
    return true;

  if (NumTerminators == 1) {
    TBB = I->getOperand(0).getMBB();
    if (I->getDesc().isConditionalBranch())
      Cond.push_back(MachineOperand::CreateImm(I->getOpcode()));
    return false;
  }

  // Two terminators: conditional followed by unconditional.
  MachineBasicBlock::iterator P = std::prev(I);
  if (P->getDesc().isConditionalBranch() && isDirectBranch(*P) &&
      I->getDesc().isUnconditionalBranch()) {
    TBB = P->getOperand(0).getMBB();
    Cond.push_back(MachineOperand::CreateImm(P->getOpcode()));
    FBB = I->getOperand(0).getMBB();
    return false;
  }
  return true;
}

bool KlaussCPUInstrInfo::reverseBranchCondition(
    SmallVectorImpl<MachineOperand> &Cond) const {
  assert(Cond.size() == 1 && "Invalid KlaussCPU branch condition");
  unsigned Inv = getInverseCondBranch(Cond[0].getImm());
  if (!Inv)
    return true;
  Cond[0].setImm(Inv);
  return false;
}

unsigned KlaussCPUInstrInfo::removeBranch(MachineBasicBlock &MBB,
                                           int *BytesRemoved) const {
  // Remove at most an unconditional branch and the conditional before it.
  unsigned Count = 0;
  int Bytes = 0;
  MachineBasicBlock::iterator I = MBB.getLastNonDebugInstr();
  while (I != MBB.end() && Count < 2 && isDirectBranch(*I)) {
    Bytes += I->getDesc().getSize();
    I->eraseFromParent();
    ++Count;
    I = MBB.getLastNonDebugInstr();
  }
  if (BytesRemoved)
    *BytesRemoved = Bytes;
  return Count;
}

unsigned KlaussCPUInstrInfo::insertBranch(MachineBasicBlock &MBB,
                                           MachineBasicBlock *TBB,
                                           MachineBasicBlock *FBB,
                                           ArrayRef<MachineOperand> Cond,
                                           const DebugLoc &DL,
                                           int *BytesAdded) const {
  assert(TBB && "insertBranch must not be called with null TBB");

  // Use JMPREL (PC-relative) for unconditional branches in PIC mode so the
  // binary can be relocated correctly when loaded at a non-zero delta.
  unsigned UncondOpc = STI.isPositionIndependent() ? KlaussCPU::JMPREL
                                                    : KlaussCPU::JMP;
  unsigned Count = 0;
  int Bytes = 0;

  if (Cond.empty()) {
    // Unconditional branch.
    assert(!FBB && "Unexpected FBB with unconditional branch");
    BuildMI(&MBB, DL, get(UncondOpc)).addMBB(TBB);
    Count = 1;
    Bytes = 8;
  } else {
    // Conditional branch: Cond[0] holds the JMPxx / JMPxxREL opcode.
    // The preceding CMPRR/CMPRV is already in the block (removeBranch only
    // removes branch instructions, not compares) so we just emit the jump.
    assert(Cond.size() == 1 && "Expected exactly one Cond operand");
    unsigned CondOpc = Cond[0].getImm();
    BuildMI(&MBB, DL, get(CondOpc)).addMBB(TBB);
    Count = 1;
    Bytes = 8;
    if (FBB) {
      BuildMI(&MBB, DL, get(UncondOpc)).addMBB(FBB);
      Count++;
      Bytes += 8;
    }
  }

  if (BytesAdded)
    *BytesAdded = Bytes;
  return Count;
}

void KlaussCPUInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                                      MachineBasicBlock::iterator I,
                                      const DebugLoc &DL,
                                      Register DestReg, Register SrcReg,
                                      bool KillSrc, bool RenamableDest,
                                      bool RenamableSrc) const {
  // SP ↔ GPR: use GETSP_R / SETSP_R.
  if (SrcReg == KlaussCPU::SP) {
    assert(KlaussCPU::GPRRegClass.contains(DestReg));
    BuildMI(MBB, I, DL, get(KlaussCPU::GETSP_R), DestReg);
    return;
  }
  if (DestReg == KlaussCPU::SP) {
    assert(KlaussCPU::GPRRegClass.contains(SrcReg));
    BuildMI(MBB, I, DL, get(KlaussCPU::SETSP_R))
        .addReg(SrcReg, getKillRegState(KillSrc));
    return;
  }
  // GPR → GPR.
  assert(KlaussCPU::GPRRegClass.contains(DestReg, SrcReg) &&
         "KlaussCPU copyPhysReg: only GPR↔GPR or SP↔GPR copies supported");
  BuildMI(MBB, I, DL, get(KlaussCPU::COPY_R), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc));
}

namespace {
// Single issue for the post-RA list scheduler.  Its default recognizer is the
// itinerary scoreboard, which a SchedMachineModel without itineraries leaves
// disabled: atIssueLimit() is then never true, and the list scheduler puts
// every ready instruction into the same cycle.  It then takes the first
// ready instructions in priority order as filling one cycle and only later
// meets the stall it could have filled (perf_baseline's ALU loop issued an
// off-path SHRV and INCR "beside" the first ADDR, and the dependent SHLV,
// XORR, ... stalled behind each other).  Counting one instruction per cycle
// makes CurCycle the issue slot, so an instruction that is not ready yet
// leaves its slot to the highest-priority ready one.  Latencies are the
// scheduler's own (KlaussCPUSchedule.td).
//
// A multiply or divide also holds EX (the RTL's ex_busy: MUL 4 cycles, see
// KlaussCPUSchedule.td), so nothing issues behind it until the hold ends.
// The list scheduler does not read ReleaseAtCycles, so without this it
// counted the slots behind a MULR as free: it put an ALU op that could have
// gone in front of the multiply behind it, where it lost the slot and stalled
// the next multiply that reads it (the Zephyr mandel loop: `addr r8 ; mulr
// r11, r8, r8` behind `mulr r2, ...`, 2 cycles per iteration on the board).
// The recognizer therefore reports a hazard in the cycles an issued
// instruction keeps its resource beyond the first (its largest
// ReleaseAtCycle - 1).  (KCUnit is a buffered resource, so the pre-RA
// scheduler counts these cycles as pressure, not as a hazard.)
class KlaussCPUPostRAHazardRecognizer : public ScheduleHazardRecognizer {
  const ScheduleDAGInstrs &DAG;
  bool Issued = false;
  unsigned BusyCycles = 0; // cycles left in which nothing may issue

  unsigned occupancy(SUnit *SU) const {
    if (!PostRAExBusy || !SU || !SU->getInstr())
      return 1;
    const TargetSchedModel *SM = DAG.getSchedModel();
    if (!SM->hasInstrSchedModel())
      return 1;
    unsigned Cycles = 1;
    const MCSchedClassDesc *SC = DAG.getSchedClass(SU);
    if (!SC || !SC->isValid())
      return 1;
    for (const MCWriteProcResEntry &PRE :
         make_range(SM->getWriteProcResBegin(SC), SM->getWriteProcResEnd(SC)))
      Cycles = std::max<unsigned>(Cycles, PRE.ReleaseAtCycle);
    return Cycles;
  }

public:
  explicit KlaussCPUPostRAHazardRecognizer(const ScheduleDAGInstrs &DAG)
      : DAG(DAG) {
    MaxLookAhead = 1;
  }
  HazardType getHazardType(SUnit *, int) override {
    return BusyCycles ? Hazard : NoHazard;
  }
  bool atIssueLimit() const override { return Issued || BusyCycles; }
  void EmitInstruction(SUnit *SU) override {
    Issued = true;
    BusyCycles = occupancy(SU) - 1;
  }
  void AdvanceCycle() override {
    if (Issued)
      Issued = false;
    else if (BusyCycles)
      --BusyCycles;
  }
  void Reset() override {
    Issued = false;
    BusyCycles = 0;
  }
};
} // end anonymous namespace

ScheduleHazardRecognizer *KlaussCPUInstrInfo::CreateTargetPostRAHazardRecognizer(
    const InstrItineraryData *II, const ScheduleDAG *DAG) const {
  if (!PostRASingleIssue)
    return TargetInstrInfo::CreateTargetPostRAHazardRecognizer(II, DAG);
  return new KlaussCPUPostRAHazardRecognizer(
      *static_cast<const ScheduleDAGInstrs *>(DAG));
}

ArrayRef<std::pair<MachineMemOperand::Flags, const char *>>
KlaussCPUInstrInfo::getSerializableMachineMemOperandTargetFlags() const {
  static const std::pair<MachineMemOperand::Flags, const char *> TargetFlags[] =
      {{KlaussCPU::MOSplitHalf, "klausscpu-split-half"}};
  return ArrayRef(TargetFlags);
}

unsigned KlaussCPUInstrInfo::getInstSizeInBytes(const MachineInstr &MI) const {
  if (MI.isMetaInstruction())
    return 0;
  const auto &FB = STI.getFeatureBits();
  switch (MI.getOpcode()) {
  case TargetOpcode::INLINEASM:
  case TargetOpcode::INLINEASM_BR: {
    const MachineFunction &MF = *MI.getMF();
    return getInlineAsmLength(MI.getOperand(0).getSymbolName(),
                              *MF.getTarget().getMCAsmInfo());
  }
  // TAILREL (KlaussCPUAsmPrinter::emitInstruction; the .td Size is the
  // 2-word worst case), compressed like a call below.
  case KlaussCPU::PseudoTAIL:
    if (FB[KlaussCPU::FeatureShortCalls])
      return 4;
    break;
  // A call to a symbol is the short CALL (CALL_SH), relaxed or thunked by
  // the assembler / lld when out of reach.  isCompressibleInst only knows
  // a literal target.
  case KlaussCPU::CALL_I:
  case KlaussCPU::CALLREL:
    if (!MI.getOperand(0).isImm() && FB[KlaussCPU::FeatureShortCalls])
      return 4;
    break;
  default:
    // A branch to a block is the short form unless the assembler relaxes it.
    if (MI.isBranch() && !MI.isIndirectBranch() && MI.getNumOperands() &&
        MI.getOperand(0).isMBB() && FB[KlaussCPU::FeatureShortBranches] &&
        get(MI.getOpcode()).getSize() == 8)
      return 4;
    break;
  }
  if (isCompressibleInst(MI, STI))
    return 4;
  return get(MI.getOpcode()).getSize();
}

//===----------------------------------------------------------------------===//
// Outlining (MachineOutliner)
//===----------------------------------------------------------------------===//
//
// CALL pushes the return address and RET pops it, so an outlined function
// needs no link register and no frame: it is entered, like any frameless
// function, with CFA = SP + 8 and the return address at [SP], which is the
// CIE's state, so it needs no CFI of its own.  The price is that SP inside
// it is 8 bytes below SP at the call site, so nothing that reads or writes
// SP may be outlined into a called function (GETSP, PUSH/POP, ADDSP,
// ENTER/LEAVE, a call: its callee would find its stack arguments and home
// area 8 bytes off).  R15 is untouched by CALL, so frame accesses
// (R15-relative) are outlined freely, also from a frameless function, which
// keeps its caller's R15 (the R15 chain then skips that function, as in any
// frameless code; the CFI describes it).  The three kinds of call:
//
//   Default:  I1 I2 I3      ->  call OUTLINED       OUTLINED: I1 I2 I3 ret
//   TailCall: I1 I2 ret     ->  tail OUTLINED       OUTLINED: I1 I2 ret
//   Thunk:    I1 I2 call f  ->  call OUTLINED       OUTLINED: I1 I2 tail f
//
// TailCall jumps, so SP is unchanged.  It ends in a plain RET or a tail
// call that is not behind a LEAVE (any LEAVE in the sequence is illegal), so
// the code runs frameless with the CIE's CFA, or in a LEAVERET: an
// epilogue, which runs in the frame of the function it was outlined from.
// Its CFI then describes that frame (CFA = R15 + 16, the caller's R15 at
// CFA - 16) and the callee-saved registers the sequence restores, at their
// slots; every candidate must restore the same registers from the same
// slots (the others are already restored).  Such a function returns to the
// caller of the one it came from, which leaves the backtrace as a tail
// call does.  In a Thunk, OUTLINED's CALL pushed the same return address
// that the call it replaces would have, so f is entered with SP and [SP] as
// before: its stack arguments (stored before the sequence through a GETSP
// outside it) and a variadic call's home area are where it looks.
//
// FLAGS: CALL and RET write no flag (pipeline_core.sv: no fmask for U_CALL /
// U_RET; nor does the emulator), so a sequence may read flags set before it
// or leave flags read after it (a flag-setting op that FlagReuse made the
// branch's compare).  The outliner adds the uses and defs to the call.
//
// CFI instructions are never outlined, nor are the prologue's FrameSetup
// instructions they follow, so -g changes nothing that may be outlined.
// (The generic outliner breaks ties between candidates by position, not by
// the instruction numbers that -g's DBG_VALUEs shift, so -g does not change
// what is outlined either.)

enum MachineOutlinerConstructionID {
  MachineOutlinerDefault,
  MachineOutlinerTailCall,
  MachineOutlinerThunk
};

static bool usesSP(const MachineInstr &MI, const TargetRegisterInfo *TRI) {
  switch (MI.getOpcode()) {
  // Not described by their operands (the return address is popped from
  // [SP]; IRET pops the interrupt frame).
  case KlaussCPU::RET_I:
  case KlaussCPU::IRET_I:
  case KlaussCPU::LEAVERET_I:
  case KlaussCPU::PUSHV:
  case KlaussCPU::PUSHV64:
    return true;
  default:
    break;
  }
  return MI.readsRegister(KlaussCPU::SP, TRI) ||
         MI.modifiesRegister(KlaussCPU::SP, TRI) ||
         MI.getDesc().hasImplicitUseOfPhysReg(KlaussCPU::SP) ||
         MI.getDesc().hasImplicitDefOfPhysReg(KlaussCPU::SP);
}

// The callee-saved registers an epilogue candidate (one that ends in
// LEAVERET) restores, with their slots as CFA offsets: the FrameDestroy
// loads in it of a callee-saved register of its function.
using CSRRestores = SmallVector<std::pair<MCRegister, int64_t>, 4>;
static CSRRestores getCSRRestores(outliner::Candidate &C) {
  const MachineFunction &MF = *C.getMF();
  const TargetFrameLowering &TFI = *MF.getSubtarget().getFrameLowering();
  CSRRestores R;
  for (const MachineInstr &MI : C) {
    if (!MI.getFlag(MachineInstr::FrameDestroy) || !MI.mayLoad() ||
        !MI.getOperand(0).isReg() || !MI.getOperand(0).isDef())
      continue;
    MCRegister Reg = MI.getOperand(0).getReg().asMCReg();
    for (const CalleeSavedInfo &CS :
         MF.getFrameInfo().getCalleeSavedInfo()) {
      if (CS.getReg() != Reg)
        continue;
      Register FrameReg;
      StackOffset Off =
          TFI.getFrameIndexReference(MF, CS.getFrameIdx(), FrameReg);
      R.push_back({Reg, Off.getFixed() -
                            static_cast<int64_t>(KlaussCPUABI::FPToCFAOffset)});
    }
  }
  llvm::sort(R);
  return R;
}

// A call that can end a Thunk candidate: a direct call to a symbol, or an
// indirect one through a register PseudoTAILIndirect accepts.
static bool isThunkableCall(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
  case KlaussCPU::CALL_I:
  case KlaussCPU::CALLREL:
    return MI.getOperand(0).isGlobal() || MI.getOperand(0).isSymbol();
  case KlaussCPU::CALLR_R:
    return KlaussCPU::GPRTCRegClass.contains(MI.getOperand(0).getReg());
  default:
    return false;
  }
}

bool KlaussCPUInstrInfo::isFunctionSafeToOutlineFrom(
    MachineFunction &MF, bool OutlineFromLinkOnceODRs) const {
  const Function &F = MF.getFunction();

  // Can F be deduplicated by the linker? If it can, don't outline from it.
  if (!OutlineFromLinkOnceODRs && F.hasLinkOnceODRLinkage())
    return false;

  // Don't outline from functions with section markings; the program could
  // expect that all the code is in the named section.
  if (F.hasSection())
    return false;

  // No outlined calls in a function without a prologue, or in one that
  // calls setjmp (a thunk would be equivalent, but keep it simple).
  if (F.hasFnAttribute(Attribute::Naked) || MF.exposesReturnsTwice())
    return false;

  return true;
}

bool KlaussCPUInstrInfo::shouldOutlineFromFunctionByDefault(
    MachineFunction &MF) const {
  return MF.getFunction().hasMinSize();
}

std::optional<std::unique_ptr<outliner::OutlinedFunction>>
KlaussCPUInstrInfo::getOutliningCandidateInfo(
    const MachineModuleInfo &MMI,
    std::vector<outliner::Candidate> &RepeatedSequenceLocs,
    unsigned MinRepeats) const {
  if (RepeatedSequenceLocs.size() < MinRepeats)
    return std::nullopt;

  // An epilogue's CFI describes the frame it runs in: keep the candidates
  // whose functions saved the registers it restores in the same slots.
  if (RepeatedSequenceLocs[0].back().getOpcode() == KlaussCPU::LEAVERET_I) {
    CSRRestores First = getCSRRestores(RepeatedSequenceLocs[0]);
    llvm::erase_if(RepeatedSequenceLocs, [&](outliner::Candidate &C) {
      return getCSRRestores(C) != First;
    });
    if (RepeatedSequenceLocs.size() < MinRepeats)
      return std::nullopt;
  }

  // Each candidate is the same sequence.
  outliner::Candidate &Candidate = RepeatedSequenceLocs[0];
  const MachineInstr &Last = Candidate.back();

  unsigned SequenceSize = 0;
  for (const MachineInstr &MI : Candidate)
    SequenceSize += getInstSizeInBytes(MI);

  // A call or a tail jump to the outlined function is one word (the short
  // CALL / TAIL; lld adds a thunk for an out-of-reach one).
  const unsigned CallSize = STI.getFeatureBits()[KlaussCPU::FeatureShortCalls]
                                ? 4
                                : 8;
  MachineOutlinerConstructionID MOCI;
  unsigned FrameOverhead;
  if (Last.isReturn()) {
    // The return (or tail jump) moves into the outlined function.
    MOCI = MachineOutlinerTailCall;
    FrameOverhead = 0;
  } else if (Last.isCall()) {
    // The call becomes the outlined function's tail jump, of the same size.
    MOCI = MachineOutlinerThunk;
    FrameOverhead = 0;
  } else {
    // A RET is added.
    MOCI = MachineOutlinerDefault;
    FrameOverhead = 4;
  }

  for (outliner::Candidate &C : RepeatedSequenceLocs)
    C.setCallInfo(MOCI, CallSize);

  return std::make_unique<outliner::OutlinedFunction>(
      RepeatedSequenceLocs, SequenceSize, FrameOverhead, MOCI);
}

outliner::InstrType
KlaussCPUInstrInfo::getOutliningTypeImpl(const MachineModuleInfo &MMI,
                                         MachineBasicBlock::iterator &MIT,
                                         unsigned Flags) const {
  MachineInstr &MI = *MIT;
  const TargetRegisterInfo *TRI = &getRegisterInfo();

  // The outlined function has no CFI of its own (see above).
  if (MI.isCFIInstruction())
    return outliner::InstrType::Illegal;

  // The prologue's stores are followed by CFI with -g; outlining them only
  // without -g would make -g change the code.
  if (MI.getFlag(MachineInstr::FrameSetup))
    return outliner::InstrType::Illegal;

  // A return or a tail call ends a TailCall candidate: RET, LEAVERET, or a
  // tail call not behind a LEAVE (any LEAVE in the sequence is illegal
  // below).
  if (MI.isReturn()) {
    switch (MI.getOpcode()) {
    case KlaussCPU::RET_I:
    case KlaussCPU::LEAVERET_I:
      return outliner::InstrType::Legal;
    case KlaussCPU::PseudoTAIL:
      return MI.getOperand(0).isGlobal() || MI.getOperand(0).isSymbol()
                 ? outliner::InstrType::Legal
                 : outliner::InstrType::Illegal;
    case KlaussCPU::PseudoTAILIndirect:
      return outliner::InstrType::Legal;
    default:
      return outliner::InstrType::Illegal; // IRET
    }
  }

  // A call can only end a candidate (a Thunk).
  if (MI.isCall())
    return isThunkableCall(MI) ? outliner::InstrType::LegalTerminator
                               : outliner::InstrType::Illegal;

  if (usesSP(MI, TRI))
    return outliner::InstrType::Illegal;

  return outliner::InstrType::Legal;
}

void KlaussCPUInstrInfo::buildOutlinedFrame(
    MachineBasicBlock &MBB, MachineFunction &MF,
    const outliner::OutlinedFunction &OF) const {
  switch (OF.FrameConstructionID) {
  case MachineOutlinerTailCall: {
    // The sequence ends in its return.  An epilogue (LEAVERET) runs in the
    // frame of the function it came from.
    if (MBB.back().getOpcode() != KlaussCPU::LEAVERET_I || !MF.needsFrameMoves())
      return;
    CFIInstBuilder CFIBuilder(MBB, MBB.begin(), MachineInstr::NoFlags);
    CFIBuilder.buildDefCFA(KlaussCPU::R15, KlaussCPUABI::FPToCFAOffset);
    CFIBuilder.buildOffset(KlaussCPU::R15,
                           -static_cast<int64_t>(KlaussCPUABI::FPToCFAOffset));
    outliner::Candidate C = OF.Candidates.front();
    for (auto [Reg, Off] : getCSRRestores(C))
      CFIBuilder.buildOffset(Reg, Off);
    return;
  }
  case MachineOutlinerThunk: {
    // The final call becomes a tail jump to the same callee.
    MachineInstr &Call = MBB.back();
    assert(Call.isCall() && "a thunk ends in a call");
    MachineOperand Target = Call.getOperand(0);
    unsigned Opc = Call.getOpcode() == KlaussCPU::CALLR_R
                       ? KlaussCPU::PseudoTAILIndirect
                       : KlaussCPU::PseudoTAIL;
    MachineInstrBuilder MIB =
        BuildMI(MBB, MBB.end(), Call.getDebugLoc(), get(Opc));
    if (Target.isReg())
      MIB.addReg(Target.getReg());
    else
      MIB.add(Target);
    Call.eraseFromParent();
    return;
  }
  default:
    MBB.insert(MBB.end(), BuildMI(MF, DebugLoc(), get(KlaussCPU::RET_I)));
    return;
  }
}

MachineBasicBlock::iterator KlaussCPUInstrInfo::insertOutlinedCall(
    Module &M, MachineBasicBlock &MBB, MachineBasicBlock::iterator &It,
    MachineFunction &MF, outliner::Candidate &C) const {
  const GlobalValue *Callee = M.getNamedValue(MF.getName());
  // The call takes a source location from the code it replaces (a Thunk's
  // that of its call), so that a debugger stepping out of a callee stops at
  // a line instead of at line 0, which lldb steps out of again.
  DebugLoc DL;
  if (C.CallConstructionID == MachineOutlinerThunk)
    DL = C.back().getDebugLoc();
  for (const MachineInstr &MI : C) {
    if (DL)
      break;
    if (!MI.isDebugInstr())
      DL = MI.getDebugLoc();
  }
  if (C.CallConstructionID == MachineOutlinerTailCall) {
    It = MBB.insert(It, BuildMI(MF, DL, get(KlaussCPU::PseudoTAIL))
                            .addGlobalAddress(Callee));
    return It;
  }
  // As LowerCall: CALLREL under PIC, CALL_I otherwise; both are emitted as
  // the short CALL.  The call instructions' FLAGS def is the ABI's clobber;
  // CALL and RET themselves write no flag, so drop it: the outliner adds a
  // def if the sequence writes FLAGS and a use if it reads flags set before
  // it.
  unsigned Opc =
      STI.isPositionIndependent() ? KlaussCPU::CALLREL : KlaussCPU::CALL_I;
  MachineInstr *Call = BuildMI(MF, DL, get(Opc)).addGlobalAddress(Callee);
  int FlagsIdx = Call->findRegisterDefOperandIdx(KlaussCPU::FLAGS,
                                                 /*TRI=*/nullptr);
  if (FlagsIdx >= 0)
    Call->removeOperand(FlagsIdx);
  It = MBB.insert(It, Call);
  return It;
}
