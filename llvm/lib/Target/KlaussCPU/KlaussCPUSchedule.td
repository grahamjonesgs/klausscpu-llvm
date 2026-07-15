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
  let CompleteModel     = 0;      // only the hot classes are modeled; rest default to 1
  // PostRAScheduler MUST stay 0: the backend has no modeled FLAGS register, so the
  // post-RA scheduler (unaware that ADDI clobbers flags and that a conditional JMP
  // reads them) hoists a flag-clobbering frame-address ADDI into a compare->branch
  // gap and breaks the branch (seen as the -O2 SHA-256 `len==0` guard misfiring ->
  // infinite copy loop).  Re-enabling this needs a real FLAGS reg (compares Defs,
  // cond-branches Uses, flag-setting ALU Defs).  Pre-RA MISched still uses the model.
  let PostRAScheduler   = 0;
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
}

def : ProcessorModel<"generic", KlaussCPUSchedModel, []>;
