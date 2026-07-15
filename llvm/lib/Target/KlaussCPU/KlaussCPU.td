// KlaussCPU target description
include "llvm/Target/Target.td"
include "KlaussCPURegisterInfo.td"
include "KlaussCPUCallingConv.td"
include "KlaussCPUSchedule.td"     // defines WriteALU/… used by InstrInfo.td below
include "KlaussCPUInstrInfo.td"

def KlaussCPUInstrInfo : InstrInfo;

// Tell Target.td pseudo-instructions (LOAD_STACK_GUARD etc.) to use GPR
// for pointer-like operands.
defm : RemapAllTargetPseudoPointerOperands<GPR>;

// Scheduling model + ProcessorModel live in KlaussCPUSchedule.td (M8).

//===----------------------------------------------------------------------===//
// Declare the target
//===----------------------------------------------------------------------===//

def KlaussCPU : Target {
  let InstructionSet = KlaussCPUInstrInfo;
}
