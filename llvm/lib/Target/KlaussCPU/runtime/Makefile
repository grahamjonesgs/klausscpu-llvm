# KlaussCPU build Makefile
#
# Usage:
#   make hello          — hello world (uart test)
#   make adventure      — text adventure game
#   make test_64bit     — KlaussCPU test suite (mixed widths, heap, recursion)
#   make expr           — recursive-descent expression evaluator
#   make bst            — binary search tree workout (heap stress)
#   make crypto         — CRC32 + SHA-256 + Base64 round-trip
#   make queens         — N-queens backtracker
#   make test_switch    — switch/case → BR_JT → JMPR_R dispatch test
#   make test_fp        — soft-FP test (links compiler-rt builtins)
#   make test_asm       — inline assembly test (step 31)
#   make test_printf    — varargs / printf test (step 34)
#   make all            — every .bin
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

# The KlaussCPU toolchain class (KlaussCPUToolChain) automatically injects
# -ffreestanding and restricts includes to Clang's own headers, so -nostdlib,
# -nostdinc, -fno-builtin, and -ffreestanding are no longer needed here.
CFLAGS  = -target $(TRIPLE) -O1

# compiler-rt builtins need an extra -I for their own headers.
# -nostdlibinc is still specified explicitly to be safe (suppresses any system
# libc headers while keeping Clang's stdint.h/stdbool.h/limits.h).
BUILTINS    = $(shell git rev-parse --show-toplevel)/compiler-rt/lib/builtins
CRT_FLAGS   = -target $(TRIPLE) -O1 -nostdlibinc \
              -I$(BUILTINS) -D__SOFTFP__

# compiler-rt soft-FP and integer-division builtins.
# __SOFTFP__ selects pure-integer paths in fix*di/fixuns*di (avoids double dep).
# fp_mode_stub.o provides __fe_getround/__fe_raise_inexact (no fenv on KlaussCPU).
# CLZ is 64-bit on hardware so all __builtin_clz expansions work correctly.

# Single-precision float
CRT_SF_NAMES = addsf3 subsf3 mulsf3 divsf3 negsf2 comparesf2 \
               floatsisf floatunsisf fixsfsi fixsfdi fixunssfsi fixunssfdi

# Double-precision float + float<->double conversions
CRT_DF_NAMES = adddf3 subdf3 muldf3 divdf3 negdf2 comparedf2 \
               floatsidf floatunsidf fixdfsi fixdfdi fixunsdfsi fixunsdfdi \
               extendsfdf2 truncdfsf2

# Integer division (needed for variable-divisor / and % on 32- and 64-bit)
CRT_INT_NAMES = udivsi3 divsi3 udivdi3 divdi3 umoddi3 moddi3 udivmoddi4

CRT_FP_NAMES = $(CRT_SF_NAMES) $(CRT_DF_NAMES) $(CRT_INT_NAMES)
CRT_FP_OBJS  = $(patsubst %, crt-%.o, $(CRT_FP_NAMES)) fp_mode_stub.o

LD_SCRIPT   = $(dir $(lastword $(MAKEFILE_LIST)))klausscpu.ld
CRT0_SRC    = $(dir $(lastword $(MAKEFILE_LIST)))crt0.c
UART_SRC    = $(dir $(lastword $(MAKEFILE_LIST)))uart_stubs.c
IO_SRC      = $(dir $(lastword $(MAKEFILE_LIST)))io_stubs.c
LIBC_SRC    = $(dir $(lastword $(MAKEFILE_LIST)))libc.c

# Common objects needed by every program
RUNTIME_OBJS = crt0.o uart_stubs.o io_stubs.o

# ── Default target ────────────────────────────────────────────────────────────

PROGRAMS = hello adventure test_64bit expr bst crypto queens test_switch test_fp test_asm test_printf

.PHONY: all clean $(PROGRAMS)

all: $(addsuffix .bin, $(PROGRAMS))

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

# compiler-rt builtins — compiled from the in-tree source with CRT_FLAGS
crt-%.o: $(BUILTINS)/%.c
	$(CC) $(CRT_FLAGS) -c -o $@ $<

# fp_mode_stub lives in this directory but needs CRT_FLAGS (-I$(BUILTINS))
fp_mode_stub.o: fp_mode_stub.c
	$(CC) $(CRT_FLAGS) -c -o $@ $<

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

expr.elf: expr.o libc.o $(RUNTIME_OBJS)
	$(LLD) -T $(LD_SCRIPT) -o $@ $(RUNTIME_OBJS) libc.o expr.o
	@echo "==> $@ built"
	@file $@

bst.elf: bst.o libc.o $(RUNTIME_OBJS)
	$(LLD) -T $(LD_SCRIPT) -o $@ $(RUNTIME_OBJS) libc.o bst.o
	@echo "==> $@ built"
	@file $@

crypto.elf: crypto.o libc.o $(RUNTIME_OBJS)
	$(LLD) -T $(LD_SCRIPT) -o $@ $(RUNTIME_OBJS) libc.o crypto.o
	@echo "==> $@ built"
	@file $@

queens.elf: queens.o libc.o $(RUNTIME_OBJS)
	$(LLD) -T $(LD_SCRIPT) -o $@ $(RUNTIME_OBJS) libc.o queens.o
	@echo "==> $@ built"
	@file $@

test_switch.elf: test_switch.o libc.o $(RUNTIME_OBJS)
	$(LLD) -T $(LD_SCRIPT) -o $@ $(RUNTIME_OBJS) libc.o test_switch.o
	@echo "==> $@ built"
	@file $@

test_fp.elf: test_fp.o $(CRT_FP_OBJS) libc.o $(RUNTIME_OBJS)
	$(LLD) -T $(LD_SCRIPT) -o $@ $(RUNTIME_OBJS) libc.o $(CRT_FP_OBJS) test_fp.o
	@echo "==> $@ built"
	@file $@

test_asm.elf: test_asm.o libc.o $(RUNTIME_OBJS)
	$(LLD) -T $(LD_SCRIPT) -o $@ $(RUNTIME_OBJS) libc.o test_asm.o
	@echo "==> $@ built"
	@file $@

test_printf.elf: test_printf.o libc.o $(RUNTIME_OBJS)
	$(LLD) -T $(LD_SCRIPT) -o $@ $(RUNTIME_OBJS) libc.o test_printf.o
	@echo "==> $@ built"
	@file $@

hello:       hello.elf       hello.bin
adventure:   adventure.elf   adventure.bin
test_64bit:  test_64bit.elf  test_64bit.bin
expr:        expr.elf        expr.bin
bst:         bst.elf         bst.bin
crypto:      crypto.elf      crypto.bin
queens:      queens.elf      queens.bin
test_switch: test_switch.elf test_switch.bin
test_fp:     test_fp.elf     test_fp.bin
test_asm:    test_asm.elf    test_asm.bin
test_printf: test_printf.elf test_printf.bin

# ── Flat binary for FPGA loader ───────────────────────────────────────────────

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@
	@echo "==> $@ ($$(wc -c < $@) bytes)"

# ── Inspect helpers ───────────────────────────────────────────────────────────

%.dump: %.elf
	$(BUILD_DIR)/bin/llvm-objdump -d --no-show-raw-insn $< | head -60

clean:
	rm -f *.o crt-*.o *.elf *.bin
