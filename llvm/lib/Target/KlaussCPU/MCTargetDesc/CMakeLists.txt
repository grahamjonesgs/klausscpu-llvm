add_llvm_component_library(LLVMKlaussCPUDesc
  KlaussCPUInstPrinter.cpp
  KlaussCPUMCAsmInfo.cpp
  KlaussCPUMCTargetDesc.cpp

  LINK_COMPONENTS
  KlaussCPUInfo
  MC
  Support
  TargetParser

  ADD_TO_COMPONENT
  KlaussCPU
)

# The tablegen outputs (KlaussCPUGen*.inc) are generated in the parent
# build directory; make them visible here and ensure they are built first.
target_include_directories(LLVMKlaussCPUDesc PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/..
)
add_dependencies(LLVMKlaussCPUDesc KlaussCPUCommonTableGen)
