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
#   make fs_demo        — FatFs + SD card demo (mount/list/read/write)
#   make all            — every .elf
#   make clean

BUILD_DIR ?= $(shell git rev-parse --show-toplevel)/build

CC  = $(BUILD_DIR)/bin/clang
LLC = $(BUILD_DIR)/bin/llc
LLD = $(BUILD_DIR)/bin/ld.lld

TARGET  = klausscpu-unknown-elf
TRIPLE  = $(TARGET)

# ── Source search paths ───────────────────────────────────────────────────────
# Runtime library sources live in src/, program sources in programs/,
# FatFs vendor sources in fatfs/.
VPATH = src:programs:fatfs

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
         -ffunction-sections -fdata-sections \
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

FATFS_DIR = $(dir $(lastword $(MAKEFILE_LIST)))fatfs

# Objects linked into every program.
RUNTIME_OBJS = crt0.o uart_stubs.o
LIBC_OBJS    = syscalls.o stdio_handles.o setjmp.o
LIBC_LINK    = $(PICOLIBC)/lib/libc.a

# FatFs objects (compiled with -I fatfs/ so ffconf.h / diskio.h are found).
# Also used by sd.c and diskio.c which reference ff.h / diskio.h.
FATFS_FLAGS = $(CFLAGS) -Os -I$(FATFS_DIR) -I$(dir $(lastword $(MAKEFILE_LIST)))
FATFS_OBJS  = ff.o ffunicode.o ffsystem.o sd.o diskio.o

# ── Default target ────────────────────────────────────────────────────────────

PROGRAMS = hello adventure test_64bit expr bst crypto queens \
           test_switch test_fp test_asm test_printf fs_demo \
           test_fatfs_printf test_big test_cache test_rtos

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

# FatFs core files — compiled with -I fatfs/ to find ffconf.h
ff.o: $(FATFS_DIR)/ff.c
	$(CC) $(FATFS_FLAGS) -c -o $@ $<

ffunicode.o: $(FATFS_DIR)/ffunicode.c
	$(CC) $(FATFS_FLAGS) -c -o $@ $<

ffsystem.o: $(FATFS_DIR)/ffsystem.c
	$(CC) $(FATFS_FLAGS) -c -o $@ $<

# SD driver and diskio porting layer also need the FatFs include path
sd.o: src/sd.c
	$(CC) $(FATFS_FLAGS) -c -o $@ $<

diskio.o: src/diskio.c
	$(CC) $(FATFS_FLAGS) -c -o $@ $<

# fs_demo needs the FatFs include path for ff.h
fs_demo.o: programs/fs_demo.c
	$(CC) $(FATFS_FLAGS) -c -o $@ $<

# ── Link helper macro ─────────────────────────────────────────────────────────
# $(call link, prog.elf, extra-objs...)
define link
	$(LLD) -T $(LD_SCRIPT) --gc-sections -o $1 \
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

fs_demo.elf: fs_demo.o $(FATFS_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(FATFS_OBJS) fs_demo.o)

# FatFs objects linked but no FatFs calls — isolation test for printf+FatFs interaction
test_fatfs_printf.o: programs/test_fatfs_printf.c
	$(CC) $(FATFS_FLAGS) -c -o $@ $<

test_fatfs_printf.elf: test_fatfs_printf.o $(FATFS_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(FATFS_OBJS) test_fatfs_printf.o)

test_big.o: programs/test_big.c
	$(CC) $(FATFS_FLAGS) -c -o $@ $<

test_big.elf: test_big.o $(FATFS_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(FATFS_OBJS) test_big.o)

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
test_printf:       test_printf.elf
fs_demo:           fs_demo.elf
test_fatfs_printf: test_fatfs_printf.elf
test_big:          test_big.elf

test_cache.elf: test_cache.o $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,test_cache.o)

test_cache: test_cache.elf

# RTOS objects (linked only into test_rtos)
RTOS_OBJS = rtos.o context_switch.o

rtos.o: src/rtos.c
	$(CC) $(CFLAGS) -c -o $@ $<

context_switch.o: src/context_switch.S
	$(CC) $(CFLAGS) -c -o $@ $<

test_rtos.o: programs/test_rtos.c
	$(CC) $(CFLAGS) -c -o $@ $<

test_rtos.elf: test_rtos.o $(RTOS_OBJS) $(LIBC_OBJS) $(RUNTIME_OBJS)
	$(call link,$@,$(RTOS_OBJS) test_rtos.o)

test_rtos: test_rtos.elf

# ── Inspect helpers ───────────────────────────────────────────────────────────

%.dump: %.elf
	$(BUILD_DIR)/bin/llvm-objdump -d --no-show-raw-insn $< | head -60

clean:
	rm -f *.o crt-*.o *.elf "adventure 2.elf" "adventure 2.o" \
	      "bst 2.elf" "bst 2.o" "crypto 2.elf" "crypto 2.o" \
	      "expr 2.elf" "expr 2.o" "hello 2.elf" "hello 2.o" \
	      "queens 2.elf" "queens 2.o" "test_64bit 2.elf" "test_64bit 2.o" \
	      "test_asm 2.elf" "test_asm 2.o" "test_fp 2.elf" "test_fp 2.o" \
	      "test_printf 2.elf" "test_printf 2.o" "test_switch 2.elf" "test_switch 2.o" \
	      "crt-adddf3 2.o" "crt-addsf3 2.o" "crt-comparedf2 2.o" "crt-comparesf2 2.o" \
	      "crt-divdf3 2.o" "crt-divdi3 2.o" "crt-divsf3 2.o" "crt-divsi3 2.o" \
	      "crt-extendsfdf2 2.o" "crt-fixdfdi 2.o" "crt-fixdfsi 2.o" \
	      "crt-fixsfdi 2.o" "crt-fixsfsi 2.o" "crt-fixunsdfdi 2.o" \
	      "crt-fixunsdfsi 2.o" "crt-fixunssfdi 2.o" "crt-fixunssfsi 2.o" \
	      "crt-floatsidf 2.o" "crt-floatsisf 2.o" "crt-floatunsidf 2.o" \
	      "crt-floatunsisf 2.o" "crt-moddi3 2.o" "crt-muldf3 2.o" "crt-mulsf3 2.o" \
	      "crt-negdf2 2.o" "crt-negsf2 2.o" "crt-subdf3 2.o" "crt-subsf3 2.o" \
	      "crt-truncdfsf2 2.o" "crt-udivdi3 2.o" "crt-udivmoddi4 2.o" \
	      "crt-udivsi3 2.o" "crt-umoddi3 2.o" "crt0 2.o" "fp_mode_stub 2.o" \
	      "io_stubs 2.o" "syscalls 2.o" "uart_stubs 2.o" 2>/dev/null; true
