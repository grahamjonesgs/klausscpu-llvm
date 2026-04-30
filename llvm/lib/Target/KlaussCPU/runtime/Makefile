# KlaussCPU build Makefile
#
# Usage:
#   make hello          — hello world (uart test)
#   make adventure      — text adventure game
#   make test_64bit     — 64-bit CPU test suite
#   make <name>.bin     — flat binary for FPGA loader
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
LIBC_SRC    = $(dir $(lastword $(MAKEFILE_LIST)))libc.c

# Common objects needed by every program
RUNTIME_OBJS = crt0.o uart_stubs.o io_stubs.o

# ── Default target ────────────────────────────────────────────────────────────

.PHONY: all clean hello adventure test_64bit

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

libc.o: $(LIBC_SRC)
	$(CC) $(CFLAGS) -c -o $@ $<

# ── Link ──────────────────────────────────────────────────────────────────────

hello.elf: hello.o libc.o $(RUNTIME_OBJS)
	$(LLD) -T $(LD_SCRIPT) -o $@ $(RUNTIME_OBJS) libc.o hello.o
	@echo "==> $@ built"
	@file $@

adventure.elf: adventure.o libc.o $(RUNTIME_OBJS)
	$(LLD) -T $(LD_SCRIPT) -o $@ $(RUNTIME_OBJS) libc.o adventure.o
	@echo "==> $@ built"
	@file $@

test_64bit.elf: test_64bit.o libc.o $(RUNTIME_OBJS)
	$(LLD) -T $(LD_SCRIPT) -o $@ $(RUNTIME_OBJS) libc.o test_64bit.o
	@echo "==> $@ built"
	@file $@

hello:       hello.elf       hello.bin
adventure:   adventure.elf   adventure.bin
test_64bit:  test_64bit.elf  test_64bit.bin

# ── Flat binary for FPGA loader ───────────────────────────────────────────────

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@
	@echo "==> $@ ($$(wc -c < $@) bytes)"

# ── Inspect helpers ───────────────────────────────────────────────────────────

%.dump: %.elf
	$(BUILD_DIR)/bin/llvm-objdump -d --no-show-raw-insn $< | head -60

clean:
	rm -f *.o *.elf *.bin
