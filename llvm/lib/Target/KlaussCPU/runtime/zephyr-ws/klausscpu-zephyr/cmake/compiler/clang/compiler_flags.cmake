include("${ZEPHYR_BASE}/cmake/compiler/clang/compiler_flags.cmake")

# KlaussCPU: -Os produces incorrect Zephyr kernel code at runtime (the scheduler
# returns garbage switch_handle values, causing IRET to jump to .text and crash).
# The backend compiles -Os without asserting, but the generated code is wrong.
# Force -O2 until the root cause (likely a PIC relocation or calling-convention
# issue under size optimisation) is diagnosed and fixed.
set_compiler_property(PROPERTY optimization_size -O2)
