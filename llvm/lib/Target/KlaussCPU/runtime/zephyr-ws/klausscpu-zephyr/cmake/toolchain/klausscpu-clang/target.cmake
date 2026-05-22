# target.cmake — KlaussCPU cross-compilation target settings.
# Called by FindTargetTools.cmake at line 88, before `bintools` target exists.
# Only set variables here; don't include any cmake that needs bintools yet.

set(CMAKE_C_COMPILER_TARGET   "klausscpu-unknown-elf")
set(CMAKE_CXX_COMPILER_TARGET "klausscpu-unknown-elf")
set(CMAKE_ASM_COMPILER_TARGET "klausscpu-unknown-elf")

set(LINKER lld)

# Extra flags applied to every compile unit.
list(APPEND TOOLCHAIN_C_FLAGS -ffreestanding -fPIC)
list(APPEND TOOLCHAIN_C_FLAGS -D__IEEE_LITTLE_ENDIAN -D_LDBL_EQ_DBL)
