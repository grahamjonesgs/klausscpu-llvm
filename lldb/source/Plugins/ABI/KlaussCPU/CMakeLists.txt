add_lldb_library(lldbPluginABIKlaussCPU PLUGIN
  ABISysV_klausscpu.cpp

  LINK_COMPONENTS
    Support
    TargetParser
  LINK_LIBS
    lldbCore
    lldbSymbol
    lldbTarget
    lldbValueObject
  )
