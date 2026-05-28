zephyr_library()
zephyr_library_sources(
    thread.c
    irq.c
    fatal.c
)
zephyr_library_sources_ifdef(CONFIG_CPU_KLAUSSCPU swap.S)
zephyr_library_sources_ifdef(CONFIG_LLEXT elf.c)
