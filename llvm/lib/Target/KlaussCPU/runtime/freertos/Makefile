# Makefile — KlaussCPU FreeRTOS port build
#
# Targets:
#   make demo         — FreeRTOS task demo (no network)
#   make net_demo     — FreeRTOS + lwIP: DHCP + NTP + HTTP in parallel tasks
#
# Prerequisites:
#   bash get-freertos.sh          (clones FreeRTOS-Kernel once)
#   cd .. && bash build-picolibc.sh   (builds picolibc with -fPIC once)

RUNTIME   := ..
BUILD_DIR := ../../../../../../build
LLVM_BIN  := $(BUILD_DIR)/bin
LWIP_DIR  := $(RUNTIME)/lwip

CC      := $(LLVM_BIN)/clang
AS      := $(LLVM_BIN)/clang
LD      := $(LLVM_BIN)/ld.lld
OBJCOPY := $(LLVM_BIN)/llvm-objcopy

TARGET  := klausscpu-unknown-elf
PICOLIBC := $(RUNTIME)/picolibc-install
KERNEL   := FreeRTOS-Kernel

# ── Common compiler flags ───────────────────────────────────────────────────

BASE_CFLAGS := \
    -target $(TARGET) \
    -O1 \
    -ffunction-sections \
    -fdata-sections \
    -fPIC \
    -ffreestanding \
    -D__IEEE_LITTLE_ENDIAN \
    -D_LDBL_EQ_DBL \
    -isystem $(PICOLIBC)/include \
    -I$(KERNEL)/include \
    -Iportable/KlaussCPU \
    -I$(RUNTIME) \
    -nostdlib

# FreeRTOS-only flags (lwipopts from freertos/ directory, no lwIP sources)
CFLAGS := $(BASE_CFLAGS) -I.

# FreeRTOS + lwIP flags:
#   -I. must come BEFORE -I$(RUNTIME)/lwip_port so that freertos/lwipopts.h
#   and freertos/arch/sys_arch.h shadow the bare-metal versions.
NET_CFLAGS := $(BASE_CFLAGS) \
    -I. \
    -I$(LWIP_DIR)/src/include \
    -I$(RUNTIME)/lwip_port \
    -Wno-macro-redefined

ASFLAGS    := $(BASE_CFLAGS) -I.
LDFLAGS    := -T $(RUNTIME)/klausscpu.ld --gc-sections -L$(PICOLIBC)/lib

# ── FreeRTOS kernel objects (shared by all targets) ─────────────────────────

KERNEL_SRCS := \
    $(KERNEL)/tasks.c \
    $(KERNEL)/queue.c \
    $(KERNEL)/list.c \
    $(KERNEL)/timers.c \
    $(KERNEL)/event_groups.c \
    $(KERNEL)/stream_buffer.c \
    $(KERNEL)/portable/MemMang/heap_4.c

KERNEL_OBJS := $(KERNEL_SRCS:.c=.o)

# ── Port objects ─────────────────────────────────────────────────────────────

PORT_OBJS := portable/KlaussCPU/port.o portable/KlaussCPU/portasm.o

# ── lwIP objects (compiled with NET_CFLAGS) ──────────────────────────────────

LWIP_CORE := \
    $(LWIP_DIR)/src/core/init.c \
    $(LWIP_DIR)/src/core/def.c \
    $(LWIP_DIR)/src/core/dns.c \
    $(LWIP_DIR)/src/core/inet_chksum.c \
    $(LWIP_DIR)/src/core/ip.c \
    $(LWIP_DIR)/src/core/mem.c \
    $(LWIP_DIR)/src/core/memp.c \
    $(LWIP_DIR)/src/core/netif.c \
    $(LWIP_DIR)/src/core/pbuf.c \
    $(LWIP_DIR)/src/core/raw.c \
    $(LWIP_DIR)/src/core/stats.c \
    $(LWIP_DIR)/src/core/sys.c \
    $(LWIP_DIR)/src/core/tcp.c \
    $(LWIP_DIR)/src/core/tcp_in.c \
    $(LWIP_DIR)/src/core/tcp_out.c \
    $(LWIP_DIR)/src/core/timeouts.c \
    $(LWIP_DIR)/src/core/udp.c

LWIP_IPV4 := \
    $(LWIP_DIR)/src/core/ipv4/acd.c \
    $(LWIP_DIR)/src/core/ipv4/autoip.c \
    $(LWIP_DIR)/src/core/ipv4/dhcp.c \
    $(LWIP_DIR)/src/core/ipv4/etharp.c \
    $(LWIP_DIR)/src/core/ipv4/icmp.c \
    $(LWIP_DIR)/src/core/ipv4/igmp.c \
    $(LWIP_DIR)/src/core/ipv4/ip4.c \
    $(LWIP_DIR)/src/core/ipv4/ip4_addr.c \
    $(LWIP_DIR)/src/core/ipv4/ip4_frag.c

LWIP_NETIF := \
    $(LWIP_DIR)/src/netif/ethernet.c

LWIP_APIS := \
    $(LWIP_DIR)/src/api/tcpip.c \
    $(LWIP_DIR)/src/api/api_lib.c \
    $(LWIP_DIR)/src/api/api_msg.c \
    $(LWIP_DIR)/src/api/err.c \
    $(LWIP_DIR)/src/api/netbuf.c \
    $(LWIP_DIR)/src/api/netdb.c \
    $(LWIP_DIR)/src/api/netifapi.c \
    $(LWIP_DIR)/src/api/sockets.c \
    $(LWIP_DIR)/src/apps/sntp/sntp.c

LWIP_SRCS  := $(LWIP_CORE) $(LWIP_IPV4) $(LWIP_NETIF) $(LWIP_APIS)
LWIP_OBJS  := $(LWIP_SRCS:.c=.lo)   # .lo = compiled with NET_CFLAGS

LWIP_PORT_SRCS := \
    $(RUNTIME)/lwip_port/ethernetif.c \
    sys_arch.c                          # FreeRTOS sys_arch

LWIP_PORT_OBJS := \
    $(RUNTIME)/lwip_port/ethernetif.lo \
    sys_arch.lo

# ── Runtime support ──────────────────────────────────────────────────────────

RUNTIME_OBJS := \
    $(RUNTIME)/crt0.o \
    $(RUNTIME)/uart_stubs.o \
    $(RUNTIME)/src/syscalls.o \
    $(RUNTIME)/stdio_handles.o \
    $(RUNTIME)/src/setjmp.o \
    $(RUNTIME)/src/mdio.o \
    $(RUNTIME)/src/eth.o

# ── Rules ─────────────────────────────────────────────────────────────────────

.PHONY: all demo net_demo clean check-kernel

all: demo net_demo

demo: demo.elf demo.bin
	@echo "==> demo.elf built"

net_demo: net_demo.elf net_demo.bin
	@echo "==> net_demo.elf built"

check-kernel:
	@test -d $(KERNEL) || { \
	    echo "ERROR: FreeRTOS kernel not found."; \
	    echo "Run:  bash get-freertos.sh"; \
	    exit 1; }

# Kernel + port — compiled with CFLAGS (no lwIP)
$(KERNEL_OBJS): %.o: %.c | check-kernel
	$(CC) $(CFLAGS) -c -o $@ $<

portable/KlaussCPU/portasm.o: portable/KlaussCPU/port.S | check-kernel
	$(AS) $(ASFLAGS) -c -o $@ $<

portable/KlaussCPU/port.o: portable/KlaussCPU/port.c | check-kernel
	$(CC) $(CFLAGS) -c -o $@ $<

# Demo objects — compiled with CFLAGS
demo/demo.o: demo/demo.c | check-kernel
	$(CC) $(CFLAGS) -c -o $@ $<

# Network demo objects — compiled with NET_CFLAGS
demo/net_demo.o: demo/net_demo.c | check-kernel
	$(CC) $(NET_CFLAGS) -c -o $@ $<

# lwIP sources — use .lo extension to distinguish from CFLAGS-compiled .o
%.lo: %.c | check-kernel
	$(CC) $(NET_CFLAGS) -c -o $@ $<

$(RUNTIME)/lwip_port/ethernetif.lo: $(RUNTIME)/lwip_port/ethernetif.c | check-kernel
	$(CC) $(NET_CFLAGS) -c -o $@ $<

sys_arch.lo: sys_arch.c | check-kernel
	$(CC) $(NET_CFLAGS) -c -o $@ $<

# Runtime objects
$(RUNTIME)/crt0.o: $(RUNTIME)/src/crt0.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(RUNTIME)/uart_stubs.o: $(RUNTIME)/src/uart_stubs.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(RUNTIME)/src/syscalls.o: $(RUNTIME)/src/syscalls.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(RUNTIME)/stdio_handles.o: $(RUNTIME)/src/stdio_handles.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(RUNTIME)/src/setjmp.o: $(RUNTIME)/src/setjmp.S
	$(AS) $(ASFLAGS) -c -o $@ $<

$(RUNTIME)/src/mdio.o: $(RUNTIME)/src/mdio.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(RUNTIME)/src/eth.o: $(RUNTIME)/src/eth.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Link
demo.elf: $(KERNEL_OBJS) $(PORT_OBJS) demo/demo.o $(RUNTIME_OBJS)
	$(LD) $(LDFLAGS) -o $@ $^ $(PICOLIBC)/lib/libc.a
	@file $@

demo.bin: demo.elf
	$(OBJCOPY) -O binary $< $@

net_demo.elf: $(KERNEL_OBJS) $(PORT_OBJS) $(LWIP_OBJS) $(LWIP_PORT_OBJS) \
              demo/net_demo.o $(RUNTIME_OBJS)
	$(LD) $(LDFLAGS) -o $@ $^ $(PICOLIBC)/lib/libc.a
	@file $@

net_demo.bin: net_demo.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f $(KERNEL_OBJS) $(PORT_OBJS) \
	      $(LWIP_OBJS) $(LWIP_PORT_OBJS) \
	      demo/demo.o demo/net_demo.o \
	      sys_arch.lo \
	      $(RUNTIME)/crt0.o $(RUNTIME)/uart_stubs.o \
	      $(RUNTIME)/src/syscalls.o $(RUNTIME)/stdio_handles.o \
	      $(RUNTIME)/src/setjmp.o \
	      $(RUNTIME)/src/mdio.o $(RUNTIME)/src/eth.o \
	      demo.elf demo.bin net_demo.elf net_demo.bin
