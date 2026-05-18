# wolfssl-toolchain.cmake — CMake cross-compile toolchain for KlaussCPU.
#
# All paths are computed from CMAKE_CURRENT_LIST_FILE so this file works
# correctly even when CMake re-reads it during try_compile() subprocesses
# (which do not inherit parent -D variables).

cmake_minimum_required(VERSION 3.16)

# ── Skip bare-metal link step during compiler probing ────────────────────────
# Without this, CMake tries to link a test executable which fails because there
# is no OS/libc to link against.  STATIC_LIBRARY avoids the linker entirely.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_COMPILER_WORKS   1 CACHE BOOL "" FORCE)
set(CMAKE_CXX_COMPILER_WORKS 1 CACHE BOOL "" FORCE)

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR klausscpu)

# ── Path computation from the toolchain file's own location ──────────────────
# This file lives at: runtime/freertos/wolfssl/wolfssl-toolchain.cmake
# Walk up to find the repo root.
get_filename_component(_WOLFSSL_DIR  "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
# _WOLFSSL_DIR  = .../runtime/freertos/wolfssl
get_filename_component(_FREERTOS_DIR "${_WOLFSSL_DIR}/.."  ABSOLUTE)
# _FREERTOS_DIR = .../runtime/freertos
get_filename_component(_RUNTIME_DIR  "${_FREERTOS_DIR}/.." ABSOLUTE)
# _RUNTIME_DIR  = .../runtime
# From runtime/ go up 5 more: KlaussCPU → Target → lib → llvm → llvm-project
get_filename_component(_REPO_ROOT "${_RUNTIME_DIR}/../../../../.." ABSOLUTE)
# _REPO_ROOT    = .../llvm-project

set(_LLVM_BUILD   "${_REPO_ROOT}/build")
set(_PICOLIBC     "${_RUNTIME_DIR}/picolibc-install")
set(_FREERTOS_INC "${_FREERTOS_DIR}")

# ── Compiler and tools ────────────────────────────────────────────────────────
set(CMAKE_C_COMPILER   "${_LLVM_BUILD}/bin/clang"    CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_LLVM_BUILD}/bin/clang++"  CACHE FILEPATH "" FORCE)
set(CMAKE_AR           "${_LLVM_BUILD}/bin/llvm-ar"  CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB       "${_LLVM_BUILD}/bin/llvm-ranlib" CACHE FILEPATH "" FORCE)

set(_TRIPLE "klausscpu-unknown-elf")

set(_FLAGS
    "-target ${_TRIPLE}"
    " -O1 -Wno-error -Wno-int-conversion -Wno-implicit-int-conversion -ffreestanding -ffunction-sections -fdata-sections -fPIC"
    " -D__IEEE_LITTLE_ENDIAN -D_LDBL_EQ_DBL"
    " -isystem ${_PICOLIBC}/include"
    " -DWOLFSSL_USER_SETTINGS"
    " -I${_WOLFSSL_DIR}"              # user_settings.h
    " -I${_FREERTOS_INC}"            # FreeRTOS.h etc.
    " -I${_FREERTOS_INC}/FreeRTOS-Kernel/include"
    " -I${_FREERTOS_INC}/portable/KlaussCPU"
    " -I${_RUNTIME_DIR}"             # mmio.h (needed by portmacro.h → FreeRTOS.h chain)
)
string(REPLACE ";" " " _FLAGS_STR "${_FLAGS}")

set(CMAKE_C_FLAGS   "${_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${_FLAGS_STR}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS  "-nostdlib" CACHE STRING "" FORCE)

# ── Search paths ──────────────────────────────────────────────────────────────
set(CMAKE_FIND_ROOT_PATH "${_PICOLIBC}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
