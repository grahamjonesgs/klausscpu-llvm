// KlaussCPU target description
include "llvm/Target/Target.td"
include "KlaussCPURegisterInfo.td"
include "KlaussCPUCallingConv.td"
include "KlaussCPUInstrInfo.td"

def KlaussCPUInstrInfo : InstrInfo;

// Tell Target.td pseudo-instructions (LOAD_STACK_GUARD etc.) to use GPR
// for pointer-like operands.
defm : RemapAllTargetPseudoPointerOperands<GPR>;

//===----------------------------------------------------------------------===//
// Scheduling model (minimal — single-issue in-order)
//===----------------------------------------------------------------------===//

def KlaussCPUSchedModel : SchedMachineModel {
  let CompleteModel      = 0;
  let IssueWidth         = 1;
  let MicroOpBufferSize  = 0;
  let LoadLatency        = 3;
  let MispredictPenalty  = 8;
  let PostRAScheduler    = 0;
}

def : ProcessorModel<"generic", KlaussCPUSchedModel, []>;

//===----------------------------------------------------------------------===//
// Declare the target
//===----------------------------------------------------------------------===//

def KlaussCPU : Target {
  let InstructionSet = KlaussCPUInstrInfo;
}
