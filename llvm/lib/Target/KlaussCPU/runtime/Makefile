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
#   make test_asm       — inline assembly test
#   make test_printf    — varargs / printf test
#   make all            — every .elf
#   make clean

BUILD_DIR ?= $(shell git rev-parse --show-toplevel)/build

CC  = $(BUILD_DIR)/bin/clang
LLC = $(BUILD_DIR)/bin/llc
LLD = $(BUILD_DIR)/bin/ld.lld

TARGET  = klausscpu-unknown-elf
TRIPLE  = $(TARGET)

# ── Source search paths ───────────────────────────────────────────────────────
# Runtime library sources live in src/, program sources in programs/.
VPATH = src:programs

# ── picolibc install path ─────────────────────────────────────────────────────
# Run ./build-picolibc.sh once to populate this directory.
PICOLIBC ?= $(dir $(lastword $(MAKEFILE_LIST)))picolibc-install

# ── Compiler flags ────────────────────────────────────────────────────────────
# -D__IEEE_LITTLE_ENDIAN and -D_LDBL_EQ_DBL are needed because the installed
# machine/ieeefp.h is the generic one; our arch-specific overrides are only
# applied during the picolibc build itself, not after installation.
CFLAGS = -target $(TRIPLE) -O1 \
         -isystem $(PICOLIBC)/include \
         -nostdlib \
         -D__IEEE_LITTLE_ENDIAN \
         -D_LDBL_EQ_DBL

# ── compiler-rt builtins (soft-FP + integer division) ────────────────────────
BUILTINS    = $(shell git rev-parse --show-toplevel)/compiler-rt/lib/builtins
CRT_FLAGS   = -target $(TRIPLE) -O1 -nostdlibinc \
              -I$(BUILTINS) -D__SOFTFP__

CRT_SF_NAMES = addsf3 subsf3 mulsf3 divsf3 negsf2 comparesf2 \
               floatsisf floatunsisf fixsfsi fixsfdi fixunssfsi fixunssfdi

CRT_DF_NAMES = adddf3 subdf3 muldf3 divdf3 negdf2 comparedf2 \
               floatsidf floatunsidf fixdfsi fixdfdi fixunsdfsi fixunsdfdi \
               extendsfdf2 truncdfsf2

CRT_INT_NAMES = udivsi3 divsi3 udivdi3 divdi3 umoddi3 moddi3 udivmoddi4

CRT_FP_NAMES = $(CRT_SF_NAMES) $(CRT_DF_NAMES) $(CRT_INT_NAMES)
CRT_FP_OBJS  = $(patsubst %, crt-%.o, $(CRT_FP_NAMES)) fp_mode_stub.o

LD_SCRIPT = $(dir $(lastword $(MAKEFILE_LIST)))klausscpu.ld

# Objects linked into every program.
RUNTIME_OBJS = crt0.o uart_stubs.o io_stubs.o
LIBC_OBJS    = syscalls.o compat.o setjmp.o
LIBC_LINK    = $(PICOLIBC)/lib/libc.a

# ── Default target ────────────────────────────────────────────────────────────

PROGRAMS = hello adventure test_64bit expr bst crypto queens \
           test_switch test_fp test_asm test_printf

.PHONY: all clean $(PROGRAMS)

all: $(addsuffix .elf, $(PROGRAMS))

# ── Compile rules ─────────────────────────────────────────────────────────────

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

# compiler-rt builtins compiled from in-tree source
crt-%.o: $(BUILTINS)/%.c
	$(CC) $(CRT_FLAGS) -c -o $@ $<

# fp_mode_stub.c lives in src/ but needs CRT_FLAGS (includes fp_mode.h from builtins)
fp_mode_stub.o: src/fp_mode_stub.c
	$(CC) $(CRT_FLAGS) -c -o $@ $<

# ── Link helper macro ─────────────────────────────────────────────────────────
# $(call link, prog.elf, extra-objs...)
define link
	$(LLD) -T $(LD_SCRIPT) -o $1 \
	    $(RUNTIME_OBJS) $(LIBC_OBJS) $2 $(LIBC_LINK)
	@echo "==> $1 built"
	@file $1
endef

# ── Program link rules ────────────────────────────────────────────────────────

hello.elf: hello.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,hello.o)

adventure.elf: adventure.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,adventure.o)

test_64bit.elf: test_64bit.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,test_64bit.o)

expr.elf: expr.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,expr.o)

bst.elf: bst.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,bst.o)

crypto.elf: crypto.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,crypto.o)

queens.elf: queens.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,queens.o)

test_switch.elf: test_switch.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,test_switch.o)

test_fp.elf: test_fp.o $(CRT_FP_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(CRT_FP_OBJS) test_fp.o)

test_asm.elf: test_asm.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,test_asm.o)

test_printf.elf: test_printf.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,test_printf.o)

# ── Phony aliases ─────────────────────────────────────────────────────────────

hello:       hello.elf
adventure:   adventure.elf
test_64bit:  test_64bit.elf
expr:        expr.elf
bst:         bst.elf
crypto:      crypto.elf
queens:      queens.elf
test_switch: test_switch.elf
test_fp:     test_fp.elf
test_asm:    test_asm.elf
test_printf: test_printf.elf

# ── Inspect helpers ───────────────────────────────────────────────────────────

%.dump: %.elf
	$(BUILD_DIR)/bin/llvm-objdump -d --no-show-raw-insn $< | head -60

clean:
	rm -f *.o crt-*.o *.elf
