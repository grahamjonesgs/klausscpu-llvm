cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(klausscpu_ssh_shell)

target_sources(app PRIVATE src/main.c)
