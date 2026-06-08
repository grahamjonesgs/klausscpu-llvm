zephyr_library()

zephyr_library_sources_ifdef(CONFIG_KLAUSSCPU_SHELL_CMDS shell_cmds.c)
zephyr_library_sources_ifdef(CONFIG_KLAUSSCPU_SHELL_CMDS shell_paths.c)

# Software wall-clock + FatFs get_fattime() (FF_FS_NORTC=0 needs it).
zephyr_library_sources_ifdef(CONFIG_FAT_FILESYSTEM_ELM wallclock.c)

# shell_cmds.c (ls) + wallclock.c include <ff.h>.  Replicate what the ELMFAT
# interface lib gives the app target: both FatFs include dirs and the override
# define that makes ffconf.h pull in zephyr_fatfs_config.h (needed for symbols
# like FS_FATFS_WINDOW_ALIGNMENT / the FF_FS_NORTC override).
if(CONFIG_FAT_FILESYSTEM_ELM)
  zephyr_library_include_directories(
    ${ZEPHYR_FATFS_MODULE_DIR}/include
    ${ZEPHYR_BASE}/modules/fatfs
  )
  zephyr_library_compile_definitions(ZEPHYR_CONFIG_OVERRIDE=zephyr_fatfs_config.h)
endif()

# Kernel symbols exported to loadable extensions (libc + getchar).
zephyr_library_sources_ifdef(CONFIG_LLEXT llext_exports.c)

if(CONFIG_KLAUSSCPU_SSH_SERVER)
  zephyr_library_sources(
    ssh_server.c
    ssh_shell_transport.c
    llext_loader.c
    sshkeys.c
    wolfssl_hw.c
  )
  zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
endif()
