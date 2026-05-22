# generic.cmake — KlaussCPU toolchain variant for Zephyr.
#
# Strategy: set COMPILER=clang, BINTOOLS=llvm, TOOLCHAIN_HOME pointing to our
# LLVM build. Then delegate to Zephyr's standard cmake/compiler/clang/generic.cmake
# (which lives under ZEPHYR_BASE, not TOOLCHAIN_ROOT) to find CMAKE_C_COMPILER.
#
# ZEPHYR_BASE is available because FindHostTools.cmake sets it before calling us.

if(NOT DEFINED KLAUSSCPU_LLVM_BIN)
  if(DEFINED ENV{KLAUSSCPU_LLVM_BIN})
    set(KLAUSSCPU_LLVM_BIN "$ENV{KLAUSSCPU_LLVM_BIN}")
  else()
    message(FATAL_ERROR
      "Set -DKLAUSSCPU_LLVM_BIN=/path/to/llvm-project/build/bin")
  endif()
endif()

if(NOT IS_DIRECTORY "${KLAUSSCPU_LLVM_BIN}")
  message(FATAL_ERROR
    "KlaussCPU LLVM bin not found: ${KLAUSSCPU_LLVM_BIN}")
endif()

message(STATUS "KlaussCPU LLVM bin: ${KLAUSSCPU_LLVM_BIN}")

set(COMPILER  clang)
set(BINTOOLS  llvm)
set(LINKER    lld)
set(TOOLCHAIN_HOME "${KLAUSSCPU_LLVM_BIN}")
set(TOOLCHAIN_HAS_NEWLIB OFF CACHE BOOL "")

# Include Zephyr's standard clang compiler detection directly.
# FindHostTools.cmake only searches TOOLCHAIN_ROOT for compiler cmake files,
# but our TOOLCHAIN_ROOT is the module dir (not ZEPHYR_BASE). Include it here
# so CMAKE_C_COMPILER is resolved before DTS preprocessing runs.
include("${ZEPHYR_BASE}/cmake/compiler/clang/generic.cmake")
include("${ZEPHYR_BASE}/cmake/bintools/llvm/generic.cmake" OPTIONAL)
