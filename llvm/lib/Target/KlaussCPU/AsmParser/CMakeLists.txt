add_llvm_component_library(LLVMKlaussCPUAsmParser
  KlaussCPUAsmParser.cpp

  LINK_COMPONENTS
  KlaussCPUDesc
  KlaussCPUInfo
  MC
  MCParser
  Support
  TargetParser

  ADD_TO_COMPONENT
  KlaussCPU
)

target_include_directories(LLVMKlaussCPUAsmParser PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
add_dependencies(LLVMKlaussCPUAsmParser KlaussCPUCommonTableGen)
