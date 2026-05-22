include("${ZEPHYR_BASE}/cmake/compiler/clang/compiler_flags.cmake")

# No overrides needed: the KlaussCPU backend now handles -g, -Os, and -Os -g
# correctly. (Earlier LLVM-23 states crashed on these; the assertions have
# since been resolved upstream of this build.)
