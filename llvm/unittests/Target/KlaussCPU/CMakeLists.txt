include_directories(
  ${LLVM_MAIN_SRC_DIR}/lib/Target/KlaussCPU
  ${LLVM_BINARY_DIR}/lib/Target/KlaussCPU
  )

set(LLVM_LINK_COMPONENTS
  KlaussCPUCodeGen
  KlaussCPUDesc
  KlaussCPUInfo
  CodeGen
  Core
  MC
  Support
  Target
  TargetParser
  )

add_llvm_target_unittest(KlaussCPUTests
  KlaussCPUABITest.cpp
  )
