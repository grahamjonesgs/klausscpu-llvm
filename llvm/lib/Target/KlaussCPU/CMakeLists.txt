add_llvm_component_group(KlaussCPU)

set(LLVM_TARGET_DEFINITIONS KlaussCPU.td)

tablegen(LLVM KlaussCPUGenRegisterInfo.inc  -gen-register-info)
tablegen(LLVM KlaussCPUGenInstrInfo.inc     -gen-instr-info)
tablegen(LLVM KlaussCPUGenSubtargetInfo.inc -gen-subtarget)
tablegen(LLVM KlaussCPUGenDAGISel.inc       -gen-dag-isel)
tablegen(LLVM KlaussCPUGenAsmWriter.inc     -gen-asm-writer)
add_public_tablegen_target(KlaussCPUCommonTableGen)

add_llvm_target(KlaussCPUCodeGen
  KlaussCPUAsmPrinter.cpp
  KlaussCPUFrameLowering.cpp
  KlaussCPUInstrInfo.cpp
  KlaussCPUISelDAGToDAG.cpp
  KlaussCPUISelLowering.cpp
  KlaussCPURegisterInfo.cpp
  KlaussCPUSubtarget.cpp
  KlaussCPUTargetMachine.cpp

  LINK_COMPONENTS
  Analysis
  AsmPrinter
  CodeGen
  CodeGenTypes
  Core
  KlaussCPUDesc
  KlaussCPUInfo
  MC
  SelectionDAG
  Support
  Target
  TargetParser
  TransformUtils

  ADD_TO_COMPONENT
  KlaussCPU
)

add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
