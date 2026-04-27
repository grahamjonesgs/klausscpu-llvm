# KlaussCPU build Makefile
#
# Usage:
#   make hello          — build hello.elf from hello.c + uart_stubs.c
#   make hello.bin      — produce a flat binary loadable on the FPGA
#   make clean
#
# Point BUILD_DIR at your llvm-project/build directory if needed.

BUILD_DIR ?= $(shell git rev-parse --show-toplevel)/build

CC      = $(BUILD_DIR)/bin/clang
LLC     = $(BUILD_DIR)/bin/llc
LLD     = $(BUILD_DIR)/bin/ld.lld
OBJCOPY = $(BUILD_DIR)/bin/llvm-objcopy

TARGET  = klausscpu-unknown-elf
TRIPLE  = $(TARGET)

CFLAGS  = -target $(TRIPLE) -O1 -nostdlib -nostdinc \
          -fno-builtin -ffreestanding

LD_SCRIPT   = $(dir $(lastword $(MAKEFILE_LIST)))klausscpu.ld
CRT0_SRC    = $(dir $(lastword $(MAKEFILE_LIST)))crt0.c
UART_SRC    = $(dir $(lastword $(MAKEFILE_LIST)))uart_stubs.c
IO_SRC      = $(dir $(lastword $(MAKEFILE_LIST)))io_stubs.c

# ── Default target ────────────────────────────────────────────────────────────

.PHONY: all clean

all: hello.elf

# ── Compile C → object ────────────────────────────────────────────────────────

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

crt0.o: $(CRT0_SRC)
	$(CC) $(CFLAGS) -c -o $@ $<

uart_stubs.o: $(UART_SRC)
	$(CC) $(CFLAGS) -c -o $@ $<

io_stubs.o: $(IO_SRC)
	$(CC) $(CFLAGS) -c -o $@ $<

# ── Link ──────────────────────────────────────────────────────────────────────

hello.elf: hello.o uart_stubs.o io_stubs.o crt0.o
	$(LLD) -T $(LD_SCRIPT) -o $@ crt0.o uart_stubs.o io_stubs.o hello.o
	@echo "==> $@ built"
	@file $@

# ── Flat binary for FPGA loader ───────────────────────────────────────────────

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@
	@echo "==> $@ ($$(wc -c < $@) bytes)"

# ── Inspect helpers ───────────────────────────────────────────────────────────

%.dump: %.elf
	$(BUILD_DIR)/bin/llvm-objdump -d --no-show-raw-insn $< | head -60

clean:
	rm -f *.o *.elf *.bin
