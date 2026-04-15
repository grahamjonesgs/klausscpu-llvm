add_llvm_component_library(LLVMKlaussCPUInfo
  KlaussCPUTargetInfo.cpp

  LINK_COMPONENTS
  Support

  ADD_TO_COMPONENT
  KlaussCPU
)
