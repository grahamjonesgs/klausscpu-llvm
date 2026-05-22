cmake_minimum_required(VERSION 3.20)

# Make include/zephyr/arch/klausscpu/arch.h and kernel_arch_*.h findable.
zephyr_include_directories(include)
zephyr_include_directories(arch/klausscpu/include)

# Zephyr automatically adds arch/klausscpu/ and soc/klausscpu/nexys_a7/
# via arch_v1.cmake and soc_v1.cmake.  Only add drivers here.
add_subdirectory(drivers/serial)
add_subdirectory(drivers/timer)
