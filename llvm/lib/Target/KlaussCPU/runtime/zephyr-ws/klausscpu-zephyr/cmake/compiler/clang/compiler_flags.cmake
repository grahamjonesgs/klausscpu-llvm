include("${ZEPHYR_BASE}/cmake/compiler/clang/compiler_flags.cmake")

# KlaussCPU: -g + optimisation triggers a backend crash in LLVM 23 DWARF emission.
# Override to disable debug info until the backend is fixed.
set_compiler_property(PROPERTY debug "")
set_compiler_property(PROPERTY nodebug "")

# KlaussCPU: -Os triggers a backend assertion crash. Use -O2 instead.
# TODO: investigate and fix the KlaussCPU backend -Os crash.
set_compiler_property(PROPERTY optimization_size -O2)
set_compiler_property(PROPERTY optimization_debug -O0)
