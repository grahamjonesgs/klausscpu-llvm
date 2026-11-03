//===-- KlaussCPUSchedule.td - KlaussCPU scheduling model -----*- tablegen -*-===//
//
// In-order, single-issue 5-stage pipeline (IF/ID/EX/MEM/WB) with
// register-sourced forwarding.  The interlocks (pipeline_core.sv:852-875)
// stall the instruction in ID, so a dependent instruction costs cycles only
// when it follows its producer too closely; the latencies below are the
// producer -> consumer issue distance at which it stops stalling:
//
//   GPR, ALU producer      2  A producer in EX stalls the consumer one cycle
//                             (hit1/hit2/hitd on b_ex); from MEM it forwards
//                             (mem_fwd_ok).  This covers every register read:
//                             ALU operands, a load/store base or index
//                             register (the EA is formed in EX), store data
//                             (use_rdd) and the FBR compare operands.
//   GPR, load              3  A load also blocks while in MEM (mem_loadlk:
//                             LOAD, LOAD32, POP, LEAVE, LEAVERET) and
//                             forwards from WB, so it needs two independent
//                             instructions behind it.
//   FLAGS                  2  A flag writer (fmask != 0) in EX stalls a flag
//                             reader (fread: conditional JMP/CALL, ADC/SBC,
//                             RCL/RCR, GETF) one cycle (flag_busy); from MEM
//                             the reader takes the WB-register flags.
//                             WriteFlags is the second write of an ALU op
//                             with a destination; a compare's FLAGS is its
//                             only def and takes WriteALU.  FBR reads no
//                             FLAGS: a compare fused into one (FuseCmpBr,
//                             after scheduling) reads its registers at the
//                             branch, which is never earlier than the
//                             compare it replaces.
//   MUL                    4  EX busy for 2 cycles, then the product travels
//                             through MEM to WB from the DSP's last stage
//                             (klaussCPU mul2); a flag reader also waits 4.
//   DIV/MOD               20  Iterative (dv.counter to 64 after a CLZ skip);
//                             one conservative value.
//   SP                        A writer (CALL, RET, PUSH, POP, ENTER, LEAVE,
//                             ADDSP) holds an SP reader in ID until it leaves
//                             WB (sp_busy), but SP is only touched by calls,
//                             frame setup and inline asm, which do not move;
//                             not modelled.
//
// The D-cache is not modelled either (the data port, KCMem below, is only
// pressure for the pre-RA scheduler).  On klaussCPU fetch-2a (the runtime's
// perf_kern) a load hit takes two issue cycles and its result reaches a
// consumer 4 cycles after it issues, so two independent instructions behind
// it run for free and a third waits; a store takes two.  A memory op behind
// another waits for the data port, and an ALU op between them costs its own
// cycle, so no order of memory and ALU ops hides that wait; the schedulers
// keep the latencies above (latency 4 for loads measured no faster).
//
// Both schedulers use this model.  The pre-RA machine scheduler is
// GenericScheduler: with MicroOpBufferSize = 0 a node whose operands are not
// ready yet waits in the pending queue, so it orders by these latencies.  (A
// variant that ranked the critical path above region register pressure was
// measured and dropped: 2-3 cycles per Dhrystone run slower on the board.)
// The post-RA list scheduler is top-down by latency, issues one instruction
// per cycle (KlaussCPUInstrInfo::CreateTargetPostRAHazardRecognizer; without
// itineraries LLVM's default recognizer sets no issue limit), issues nothing
// behind a MUL/DIV for its ReleaseAtCycles (the same recognizer: the list
// scheduler does not read them) and renames
// registers to break anti-dependencies on the critical path
// (KlaussCPUSubtarget::getAntiDepBreakMode).  A loop counter read by the
// exit branch would still sit directly in front of it after its last use;
// KlaussCPUEarlyIVInc moves the increment to the top of the loop first.
//
//===----------------------------------------------------------------------===//

def KlaussCPUSchedModel : SchedMachineModel {
  let IssueWidth        = 1;      // single-issue in-order
  let MicroOpBufferSize = 0;      // in-order: an operand not ready is a stall
  let LoadLatency       = 3;      // load->use (forwards from WB only)
  let MispredictPenalty = 7;      // a mispredicted FBR (board: ~6.9); no pass in
                                  // this pipeline reads it (no early if-conversion)
  let CompleteModel     = 1;      // every KlaussCPU instr resolves a SchedRW: the
                                  // KCInst32/64/96 base classes default to [WriteALU]
                                  // and the memory/branch/SP/mul/div formats override,
                                  // so nothing falls back to the latency-1 default.
  // The post-RA list scheduler runs from -O1 up
  // (KlaussCPUSubtarget::getOptLevelToEnablePostRAScheduler).  It respects the
  // modelled FLAGS def/use (KlaussCPURegisterInfo.td), so it never moves a
  // flag writer into a compare -> branch gap.  `-mllvm -post-RA-scheduler=false`
  // (or `-mcpu=no-sched`) turns it off.
  let PostRAScheduler   = 1;
}

// SchedWrites referenced from KlaussCPUInstrInfo.td (must precede its include).
def WriteALU      : SchedWrite;  // single-cycle integer op / shift / compare / move
def WriteFlags    : SchedWrite;  // FLAGS of an ALU op that also writes a register
def WriteLoad     : SchedWrite;  // memory load
def WriteLoad32   : SchedWrite;  // MEMGET32: a load the ALU bypass does not take
def ReadLoadByp   : SchedRead;   // an ALU/BOOL/compare operand (see ReadAdvance)
def WriteStore    : SchedWrite;  // memory store (no GPR result)
def WriteMul      : SchedWrite;  // 64-bit multiply
def WriteMulFlags : SchedWrite;  // its FLAGS
def WriteDiv      : SchedWrite;  // 64-bit divide / modulo
def WriteDivFlags : SchedWrite;  // its FLAGS
def WriteBranch   : SchedWrite;  // branch / jump / call (issue-slot occupancy)
def WriteSP       : SchedWrite;  // stack push/pop/addsp (SP-serialized)

let SchedModel = KlaussCPUSchedModel in {
  def KCUnit : ProcResource<1>;                 // the single EX / issue slot
  // The data port.  Only the pre-RA scheduler reads it, as resource pressure:
  // the post-RA hazard recognizer gives a load or store one issue slot
  // (KlaussCPUInstrInfo.cpp explains why).  The resource is kept buffered on
  // purpose: as an unbuffered one (BufferSize = 0) the pre-RA GenericScheduler
  // also reordered around it, which on the board cost nettle-aes +2.2% (and
  // its .text +3.4%) and statemate +1.2% (klaussCPU 8e6b8e5).
  def KCMem  : ProcResource<1>;

  def : WriteRes<WriteALU,    [KCUnit]> { let Latency = 2; }
  // A load's result is at the consumer 4 cycles after the load issues on
  // fetch-2a, but latency 4 was no faster on the board (pre-RA it cost
  // nettle-aes 5%, post-RA only it was neutral); 1a stage 3 aims for 3.
  def : WriteRes<WriteLoad,   [KCUnit, KCMem]> { let Latency = 3; }
  def : WriteRes<WriteLoad32, [KCUnit, KCMem]> { let Latency = 3; }
  // klaussCPU ldbyp: an ALU, boolean or compare op (U_ALU, U_BOOL; the
  // compare a fused branch is made of) takes a fast-path load's data from WB
  // on its register operands, one cycle before the dispatch forward: load-use
  // distance 2.  Not MEMGET32 (U_LOAD32), and not shifts, bit ops, moves,
  // MUL/DIV, addresses or store data, which keep 3.  The consumers are listed
  // in KlaussCPUScheduleBypass.td, which needs the instruction records.
  def : ReadAdvance<ReadLoadByp, 1, [WriteLoad]>;
  // A store takes one issue slot (klaussCPU stq: a second store entry, so a
  // store finds a free port unless 3 or more come back to back).
  def : WriteRes<WriteStore,  [KCUnit, KCMem]> { let Latency = 1;
                                                 let ReleaseAtCycles = [1, 2]; }
  def : WriteRes<WriteBranch, [KCUnit]> { let Latency = 1; }
  def : WriteRes<WriteSP,     [KCUnit]> { let Latency = 2; }
  // EX-serializing multi-cycle ops: hold the unit so nothing issues behind
  // them (the RTL's ex_busy hold).  A multiply occupies EX 2 cycles and its
  // product reaches a consumer 4 cycles after it issues (klaussCPU mul2).
  def : WriteRes<WriteMul,    [KCUnit]> { let Latency = 4;  let ReleaseAtCycles = [2];  }
  def : WriteRes<WriteDiv,    [KCUnit]> { let Latency = 20; let ReleaseAtCycles = [20]; }
  // The FLAGS half of an instruction already counted above: no issue slot.
  def : WriteRes<WriteFlags,    []> { let Latency = 2;  let NumMicroOps = 0; }
  def : WriteRes<WriteMulFlags, []> { let Latency = 4;  let NumMicroOps = 0; }
  def : WriteRes<WriteDivFlags, []> { let Latency = 20; let NumMicroOps = 0; }

  // The target-independent COPY pseudo survives to the scheduler as a register
  // move (it lowers to COPY_R = WriteALU).  CompleteModel=1 requires it to carry
  // sched info; model it as the ALU move it becomes.
  def : InstRW<[WriteALU], (instrs COPY)>;
}

def : ProcessorModel<"generic", KlaussCPUSchedModel,
                     [FeatureShortForms, FeatureShortBranches,
                      FeatureShortCalls]>;

// A/B baseline (spec §7.1): `-mcpu=no-sched` selects NoSchedModel — no per-op
// latencies, PostRAScheduler off — so the whole M8 transform can be turned off with
// one flag (a whole SchedMachineModel can't be gated by a SubtargetFeature in LLVM,
// so a second processor is the idiomatic A/B lever).  The default CPU is forced to
// "generic" in KlaussCPUSubtarget, so the model stays ON by default; -mcpu=no-sched
// overrides it.  The TTI unroll re-tune keys on the same switch
// (KlaussCPUTTIImpl::getUnrollingPreferences checks getSchedModel().hasInstrSchedModel()),
// so -mcpu=no-sched reverts both halves of M8 together.
def : ProcessorModel<"no-sched", NoSchedModel,
                     [FeatureShortForms, FeatureShortBranches,
                      FeatureShortCalls]>;
