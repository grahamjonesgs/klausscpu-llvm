add_llvm_component_library(LLVMKlaussCPUDisassembler
  KlaussCPUDisassembler.cpp

  LINK_COMPONENTS
  KlaussCPUDesc
  KlaussCPUInfo
  MC
  MCDisassembler
  Support

  ADD_TO_COMPONENT
  KlaussCPU
)

target_include_directories(LLVMKlaussCPUDisassembler PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
add_dependencies(LLVMKlaussCPUDisassembler KlaussCPUCommonTableGen)
