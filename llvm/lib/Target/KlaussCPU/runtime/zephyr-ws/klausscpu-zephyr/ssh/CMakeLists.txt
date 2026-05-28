zephyr_library()

zephyr_library_sources_ifdef(CONFIG_KLAUSSCPU_SHELL_CMDS shell_cmds.c)

# Kernel symbols exported to loadable extensions (libc + getchar).
zephyr_library_sources_ifdef(CONFIG_LLEXT llext_exports.c)

if(CONFIG_KLAUSSCPU_SSH_SERVER)
  zephyr_library_sources(
    ssh_server.c
    llext_loader.c
    sshkeys.c
    wolfssl_hw.c
  )
  zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
endif()
