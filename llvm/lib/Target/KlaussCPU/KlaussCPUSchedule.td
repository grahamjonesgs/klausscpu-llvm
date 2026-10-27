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
//   MUL                    5  EX busy for 4 cycles (mul_cnt 0..3, md_done,
//                             pipeline_core.sv:918), then forwards from MEM.
//   DIV/MOD               20  Iterative (dv.counter to 64 after a CLZ skip);
//                             one conservative value.
//   SP                        A writer (CALL, RET, PUSH, POP, ENTER, LEAVE,
//                             ADDSP) holds an SP reader in ID until it leaves
//                             WB (sp_busy), but SP is only touched by calls,
//                             frame setup and inline asm, which do not move;
//                             not modelled.
//
// The D-cache is not modelled either: every load and store hit holds MEM
// for ~4 (load) / ~3 (store) cycles and freezes the pipeline behind it
// (mem_busy, pipeline_core.sv:1235-1237, 1881).  That cost is the same in
// any order, and the load-use distance stays 3 under it (L;I1;I2;C needs no
// bubble), so it changes no schedule.  Measured on the board: perf_haz.
//
// Both schedulers use this model.  The pre-RA machine scheduler is
// GenericScheduler: with MicroOpBufferSize = 0 a node whose operands are not
// ready yet waits in the pending queue, so it orders by these latencies.  (A
// variant that ranked the critical path above region register pressure was
// measured and dropped: 2-3 cycles per Dhrystone run slower on the board.)
// The post-RA list scheduler is top-down by latency, issues one instruction
// per cycle (KlaussCPUInstrInfo::CreateTargetPostRAHazardRecognizer; without
// itineraries LLVM's default recognizer sets no issue limit) and renames
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
  let MispredictPenalty = 3;      // taken-redirect IFB refill via the I-cache (post-M7)
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
def WriteStore    : SchedWrite;  // memory store (no GPR result)
def WriteMul      : SchedWrite;  // 64-bit multiply
def WriteMulFlags : SchedWrite;  // its FLAGS
def WriteDiv      : SchedWrite;  // 64-bit divide / modulo
def WriteDivFlags : SchedWrite;  // its FLAGS
def WriteBranch   : SchedWrite;  // branch / jump / call (issue-slot occupancy)
def WriteSP       : SchedWrite;  // stack push/pop/addsp (SP-serialized)

let SchedModel = KlaussCPUSchedModel in {
  def KCUnit : ProcResource<1>;                 // the single EX / issue slot

  def : WriteRes<WriteALU,    [KCUnit]> { let Latency = 2; }
  def : WriteRes<WriteLoad,   [KCUnit]> { let Latency = 3; }
  def : WriteRes<WriteStore,  [KCUnit]> { let Latency = 1; }
  def : WriteRes<WriteBranch, [KCUnit]> { let Latency = 1; }
  def : WriteRes<WriteSP,     [KCUnit]> { let Latency = 2; }
  // EX-serializing multi-cycle ops: hold the unit so nothing issues behind
  // them (the RTL's ex_busy hold).  A multiply occupies EX 4 cycles and its
  // result forwards one cycle later.
  def : WriteRes<WriteMul,    [KCUnit]> { let Latency = 5;  let ReleaseAtCycles = [4];  }
  def : WriteRes<WriteDiv,    [KCUnit]> { let Latency = 20; let ReleaseAtCycles = [20]; }
  // The FLAGS half of an instruction already counted above: no issue slot.
  def : WriteRes<WriteFlags,    []> { let Latency = 2;  let NumMicroOps = 0; }
  def : WriteRes<WriteMulFlags, []> { let Latency = 5;  let NumMicroOps = 0; }
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
