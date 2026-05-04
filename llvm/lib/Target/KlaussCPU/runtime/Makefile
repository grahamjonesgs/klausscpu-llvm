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

CFLAGS  = -target $(TRIPLE) -O1 -nostdlib -nostdinc \
          -fno-builtin -ffreestanding

# compiler-rt builtins live in-tree; use -nostdlibinc (keeps Clang's own
# stdint.h / stdbool.h / limits.h) rather than -nostdinc.
BUILTINS    = $(shell git rev-parse --show-toplevel)/compiler-rt/lib/builtins
CRT_FLAGS   = -target $(TRIPLE) -O1 -nostdlib -nostdlibinc \
              -fno-builtin -ffreestanding -I$(BUILTINS) \
              -D__SOFTFP__

# Single-precision soft-FP from compiler-rt.
# __SOFTFP__ selects the pure-integer path in fixsfdi/fixunssfdi (no double).
# fp_mode_stub.o provides __fe_getround / __fe_raise_inexact stubs.
#
# addsf3 / subsf3 / negsf2 are intentionally excluded: compiler-rt's addsf3
# uses rep_clz() → __builtin_clz(), which the backend expands as
# "shlr r, x, 32; clz r" (64-bit CLZ assumed).  The hardware CLZ instruction
# only examines the lower 32 bits of the register, so after the shift the
# result is clz(0)=32 rather than the correct count → wrong renormalisation.
# softfp_addsub.o provides those three functions via a while-loop that
# requires no CLZ instruction.
CRT_FP_NAMES = mulsf3 divsf3 comparesf2 \
               floatsisf floatunsisf fixsfsi fixsfdi fixunssfsi fixunssfdi
CRT_FP_OBJS  = $(patsubst %, crt-%.o, $(CRT_FP_NAMES)) \
               fp_mode_stub.o softfp_addsub.o

LD_SCRIPT   = $(dir $(lastword $(MAKEFILE_LIST)))klausscpu.ld
CRT0_SRC    = $(dir $(lastword $(MAKEFILE_LIST)))crt0.c
UART_SRC    = $(dir $(lastword $(MAKEFILE_LIST)))uart_stubs.c
IO_SRC      = $(dir $(lastword $(MAKEFILE_LIST)))io_stubs.c
LIBC_SRC    = $(dir $(lastword $(MAKEFILE_LIST)))libc.c

# Common objects needed by every program
RUNTIME_OBJS = crt0.o uart_stubs.o io_stubs.o

# ── Default target ────────────────────────────────────────────────────────────

PROGRAMS = hello adventure test_64bit expr bst crypto queens test_switch test_fp

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

# fp_mode_stub and softfp_addsub live in this directory; compile with CFLAGS
# (no -I$(BUILTINS) needed — they are self-contained)
fp_mode_stub.o: fp_mode_stub.c
	$(CC) $(CRT_FLAGS) -c -o $@ $<

softfp_addsub.o: softfp_addsub.c
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

hello:       hello.elf       hello.bin
adventure:   adventure.elf   adventure.bin
test_64bit:  test_64bit.elf  test_64bit.bin
expr:        expr.elf        expr.bin
bst:         bst.elf         bst.bin
crypto:      crypto.elf      crypto.bin
queens:      queens.elf      queens.bin
test_switch: test_switch.elf test_switch.bin
test_fp:     test_fp.elf     test_fp.bin

# ── Flat binary for FPGA loader ───────────────────────────────────────────────

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@
	@echo "==> $@ ($$(wc -c < $@) bytes)"

# ── Inspect helpers ───────────────────────────────────────────────────────────

%.dump: %.elf
	$(BUILD_DIR)/bin/llvm-objdump -d --no-show-raw-insn $< | head -60

clean:
	rm -f *.o crt-*.o *.elf *.bin
