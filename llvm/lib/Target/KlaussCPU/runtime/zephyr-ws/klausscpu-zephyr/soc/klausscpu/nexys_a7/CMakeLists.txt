# Force the clang target triple before compiler detection finalises.
set(CMAKE_C_COMPILER_TARGET   "klausscpu-unknown-elf" CACHE STRING "" FORCE)
set(CMAKE_CXX_COMPILER_TARGET "klausscpu-unknown-elf" CACHE STRING "" FORCE)
set(CMAKE_ASM_COMPILER_TARGET "klausscpu-unknown-elf" CACHE STRING "" FORCE)

# Point Zephyr at our custom linker script.
set(SOC_LINKER_SCRIPT
    ${CMAKE_CURRENT_LIST_DIR}/linker.ld CACHE STRING "" FORCE)

zephyr_library()
