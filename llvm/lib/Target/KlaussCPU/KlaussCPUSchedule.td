//===-- KlaussCPUSchedule.td - KlaussCPU scheduling model -----*- tablegen -*-===//
//
// In-order 5-stage pipeline (IF/ID/EX/MEM/WB) with M6 register-sourced
// forwarding.  Latencies are producer->consumer *effective* latencies derived
// from the RTL (fpga/KlaussCPU pipeline_core.sv) and board-measured (M5-M7):
//   ALU  = 2  (single-cycle op; a producer-in-EX consumer pays 1 bubble, then
//              forwards from MEM -- so distance>=2 is stall-free)
//   Load = 3  (loads do NOT forward from MEM; a dependent use stalls through
//              EX+MEM and forwards from WB)
//   Mul  = 5  (EX held busy 4 cycles; measured avglat 5.0c)
//   Div  = 20 (iterative to 64 steps after a CLZ-skip; EX-serializing; a single
//              conservative value -- exact latency barely moves scheduling)
// This model is the M8 lever: without it llvm assumes latency 1 for every op and
// never spreads a dependent chain, which is why DATA (GPR RAW) stalls are
// 18-61% of cycles post-M7 (perf/m7/haz_real_m7a.csv).  See M8_SCHED_MODEL_SPEC.md.
//
//===----------------------------------------------------------------------===//

def KlaussCPUSchedModel : SchedMachineModel {
  let IssueWidth        = 1;      // single-issue in-order
  let MicroOpBufferSize = 0;      // in-order, no reorder buffer
  let LoadLatency       = 3;      // load->use (forwards from WB only)
  let MispredictPenalty = 3;      // taken-redirect IFB refill via the I-cache (post-M7)
  let CompleteModel     = 1;      // every KlaussCPU instr resolves a SchedRW (§4): the
                                  // KCInst32/64/96 base classes default to [WriteALU]
                                  // and the memory/branch/SP/mul/div formats override,
                                  // so nothing falls back to the latency-1 default.
  // PostRAScheduler ON (M8 §3): the post-RA list scheduler spreads dependent chains
  // using the latencies below.  The pre-RA MISched (enabled via
  // KlaussCPUSubtarget::enableMachineScheduler) does most of the spreading with full
  // freedom; the post-RA pass then catches spills/reloads and frame-address
  // materialisation the pre-RA pass never sees (those appear only after regalloc/PEI).
  // This was previously forced OFF because the backend had no modeled FLAGS register,
  // so the scheduler could hoist a flag-clobbering frame-address ADDI into a
  // compare->branch gap and break the branch (the -O2 SHA-256 `len==0` guard misfired
  // -> infinite copy loop).  The FLAGS register is now modeled
  // (KlaussCPURegisterInfo.td): Defs=[FLAGS] on every flag-setting ALU op incl. ADDI,
  // Uses=[FLAGS] on conditional branches (and ADC/SBC/rotate-through-carry).  That
  // makes the hazard a real scheduling dependency the post-RA scheduler respects, so
  // the original blocker is gone.  Escape hatch if a regression appears:
  // -enable-post-ra-scheduler=false (or revert this to 0).
  let PostRAScheduler   = 1;
}

// SchedWrites referenced from KlaussCPUInstrInfo.td (must precede its include).
// Shift/compare fold into WriteALU (identical latency); no WriteFlags — the
// backend has no modeled flag register (flag->branch order is held by the
// explicit CMP's hasSideEffects), so there is nothing to attach it to.
def WriteALU    : SchedWrite;  // single-cycle integer op / shift / compare / move
def WriteLoad   : SchedWrite;  // memory load
def WriteStore  : SchedWrite;  // memory store (no GPR result)
def WriteMul    : SchedWrite;  // 64-bit multiply
def WriteDiv    : SchedWrite;  // 64-bit divide / modulo
def WriteBranch : SchedWrite;  // branch / jump / call (issue-slot occupancy)
def WriteSP     : SchedWrite;  // stack push/pop/addsp (SP-serialized)

let SchedModel = KlaussCPUSchedModel in {
  def KCUnit : ProcResource<1>;                 // the single EX / issue slot

  def : WriteRes<WriteALU,    [KCUnit]> { let Latency = 2; }
  def : WriteRes<WriteLoad,   [KCUnit]> { let Latency = 3; }
  def : WriteRes<WriteStore,  [KCUnit]> { let Latency = 1; }
  def : WriteRes<WriteBranch, [KCUnit]> { let Latency = 1; }
  def : WriteRes<WriteSP,     [KCUnit]> { let Latency = 2; }
  // EX-serializing multi-cycle ops: hold the unit so nothing packs behind them
  // (matches the RTL mul/div busy interlock).  This LLVM uses ReleaseAtCycles
  // (formerly ResourceCycles).
  def : WriteRes<WriteMul,    [KCUnit]> { let Latency = 5;  let ReleaseAtCycles = [5];  }
  def : WriteRes<WriteDiv,    [KCUnit]> { let Latency = 20; let ReleaseAtCycles = [20]; }

  // The target-independent COPY pseudo survives to the scheduler as a register
  // move (it lowers to COPY_R = WriteALU).  CompleteModel=1 requires it to carry
  // sched info; model it as the ALU move it becomes.
  def : InstRW<[WriteALU], (instrs COPY)>;
}

def : ProcessorModel<"generic", KlaussCPUSchedModel, []>;

// A/B baseline (spec §7.1): `-mcpu=no-sched` selects NoSchedModel — no per-op
// latencies, PostRAScheduler off — so the whole M8 transform can be turned off with
// one flag (a whole SchedMachineModel can't be gated by a SubtargetFeature in LLVM,
// so a second processor is the idiomatic A/B lever).  The default CPU is forced to
// "generic" in KlaussCPUSubtarget, so the model stays ON by default; -mcpu=no-sched
// overrides it.  The TTI unroll re-tune keys on the same switch
// (KlaussCPUTTIImpl::getUnrollingPreferences checks getSchedModel().hasInstrSchedModel()),
// so -mcpu=no-sched reverts both halves of M8 together.
def : ProcessorModel<"no-sched", NoSchedModel, []>;
