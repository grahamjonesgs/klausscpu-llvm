// KlaussCPU target description
include "llvm/Target/Target.td"

//===----------------------------------------------------------------------===//
// Subtarget features
//
// Both are on in every processor; -mllvm -klausscpu-short-forms=false /
// -klausscpu-short-branches=false turn them off (KlaussCPUMCTargetDesc.cpp).
//===----------------------------------------------------------------------===//

// ISA v3 A3-A5: 1-word forms of the immediate ALU ops, SETR, CMPRV and the
// indexed loads/stores whose immediate fits word0[19:12].
def FeatureShortForms
    : SubtargetFeature<"short-forms", "HasShortForms", "true",
                       "ISA v3 short 1-word immediate forms">;
def HasShortForms : Predicate<"Subtarget->hasShortForms()">,
                    AssemblerPredicate<(all_of FeatureShortForms),
                                       "short-forms">;

// ISA v3 A1: 1-word PC-relative branches.  Turning the short forms off turns
// these off too.
def FeatureShortBranches
    : SubtargetFeature<"short-branches", "HasShortBranches", "true",
                       "ISA v3 short 1-word PC-relative branches",
                       [FeatureShortForms]>;
def HasShortBranches : Predicate<"Subtarget->hasShortBranches()">,
                       AssemblerPredicate<(all_of FeatureShortBranches),
                                          "short-branches">;

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

// The CompressPat emitter needs the subtarget passed to the print methods.
def KlaussCPUAsmWriter : AsmWriter {
  let PassSubtarget = 1;
}

def KlaussCPUAsmParser : AsmParser {
  // Registers also match their letter names a..p (the AltNames).
  let ShouldEmitMatchRegisterAltName = 1;
}

def KlaussCPU : Target {
  let InstructionSet = KlaussCPUInstrInfo;
  let AssemblyWriters = [KlaussCPUAsmWriter];
  let AssemblyParsers = [KlaussCPUAsmParser];
  // No instruction has hidden register constraints, so post-RA passes
  // (MachineCopyPropagation, the post-RA scheduler's anti-dependence breaker)
  // may rename registers.  Without this every instruction is emitted with
  // ExtraSrc/DefRegAllocReq, which forbids renaming.
  let AllowRegisterRenaming = 1;
}
