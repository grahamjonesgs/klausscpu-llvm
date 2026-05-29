# KlaussCPU LLVM Backend

Custom LLVM backend for the KlaussCPU — a home-designed 64-bit CPU implemented
in Verilog/SystemVerilog on a Digilent Nexys A7 (Artix-7) FPGA.

---

## Repository layout

```
llvm/lib/Target/KlaussCPU/
├── KlaussCPU*.td / *.h / *.cpp   LLVM backend source
├── MCTargetDesc/                  MC layer (encoder, assembler backend)
├── TargetInfo/                    Target registration
├── runtime/                       C runtime + demo programs
│   ├── Makefile                   build all programs
│   ├── klausscpu.ld               linker script (128 MiB flat RAM)
│   ├── mmio.h                     MMIO peripheral register map + inline helpers
│   ├── build-picolibc.sh          one-time picolibc build script
│   ├── picolibc-cross.ini         meson cross-compile template (reference)
│   ├── src/                       runtime library (linked into every program)
│   │   ├── crt0.c                 startup: BSS clear, call main
│   │   ├── uart_stubs.c           UART TX/RX with CR+LF conversion
│   │   ├── io_stubs.c             hardware delay (DELAYV/DELAYR CPU instruction)
│   │   ├── syscalls.c             picolibc glue: stdin/stdout/stderr, _sbrk
│   │   ├── compat.c               old print_str/newline API → uart_putc
│   │   ├── setjmp.S               setjmp/longjmp (assembly)
│   │   └── fp_mode_stub.c         compiler-rt FP mode stub
│   ├── programs/                  test and demo programs
│   │   ├── hello.c                UART smoke test
│   │   ├── adventure.c            text adventure game
│   │   ├── test_64bit.c           integer/memory/heap test suite
│   │   ├── expr.c                 recursive expression evaluator
│   │   ├── bst.c                  binary search tree + heap stress
│   │   ├── crypto.c               CRC32 / SHA-256 / Base64
│   │   ├── queens.c               N-queens backtracker
│   │   ├── test_switch.c          switch → BR_JT → JMPR_R
│   │   ├── test_fp.c              soft-FP smoke test
│   │   ├── test_asm.c             inline assembly test
│   │   └── test_printf.c          varargs / printf test
│   ├── freertos/                  FreeRTOS port (see Step 4)
│   │   ├── portable/KlaussCPU/    arch port (port.c, port.S, portmacro.h)
│   │   ├── FreeRTOSConfig.h       kernel configuration
│   │   ├── get-freertos.sh        clones FreeRTOS-Kernel/ from upstream
│   │   ├── demo/                  rtos smoke test (4 tasks, 7-seg display)
│   │   ├── telnet/ , ssh/         net demos (lwIP + wolfSSL/wolfSSH)
│   │   └── Makefile               builds demo.elf, console_demo.elf, net_demo.elf
│   └── zephyr-ws/                 Zephyr 3.7 LTS port (see Step 5)
│       └── klausscpu-zephyr/      out-of-tree module
│           ├── arch/klausscpu/    __start, context switch, timer ISR
│           ├── soc/klausscpu/     SoC + linker script
│           ├── boards/klausscpu/  Nexys A7 board files
│           ├── drivers/           polled UART + sys_clock
│           └── README.md          detailed build/load instructions
├── CLAUDE.md                      architecture reference + build notes
├── FPGA_FIXES_HISTORY.md          hardware errata log
└── RTOS_NOTES.md                  RTOS/interrupt notes (future work)
```

---

## Prerequisites

| Tool | Purpose |
|------|---------|
| `cmake` ≥ 3.20 | LLVM configure |
| `ninja` | LLVM build |
| `git` | clone / submodule |
| `meson` ≥ 0.63 | picolibc configure (one-time) |
| Python 3 | meson dependency |

All compiler tools (`clang`, `llvm-ar`, `llvm-nm`, etc.) are built from this
repo — no external toolchain needed.

---

## Step 1 — Build the LLVM backend, clang, and lld

From `llvm-project/`:

```bash
mkdir -p build && cd build

cmake -G Ninja ../llvm \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLLVM_TARGETS_TO_BUILD="X86" \
  -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD="KlaussCPU" \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_USE_SPLIT_DWARF=ON \
  -DLLVM_OPTIMIZED_TABLEGEN=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

ninja
```

This builds everything: `llc`, `clang`, `ld.lld`, `llvm-ar`, `llvm-objcopy`,
and the KlaussCPU backend. Takes 1–3 hours on a first build; subsequent
incremental builds targeting only KlaussCPU components are much faster:

```bash
ninja LLVMKlaussCPUCodeGen llc clang
```

> **Note:** `X86` is included because LLVM's host tools (TableGen, etc.)
> require a native code generator. It does not affect the KlaussCPU output.

---

## Step 2 — Build picolibc (one-time)

picolibc is the C standard library. It is not in this repository — the build
script clones it from GitHub, patches it for KlaussCPU, and installs it into
`runtime/picolibc-install/`.

```bash
cd llvm/lib/Target/KlaussCPU/runtime
./build-picolibc.sh
```

This requires `meson`, `ninja`, and the LLVM tools built in Step 1.

The script:
1. Clones `https://github.com/picolibc/picolibc.git` into `picolibc-src/`
2. Creates machine-specific headers for KlaussCPU (`machine/ieeefp.h`,
   `machine/setjmp.h`) that the generic picolibc headers don't provide
3. Configures with meson using our cross-compile settings
4. Builds and installs to `picolibc-install/`

Re-run only when you want to update picolibc to a newer upstream commit.

### compiler-rt soft-FP builtins

The floating-point and integer-division runtime (`__addsf3`, `__adddf3`,
`__udivdi3`, etc.) comes from **compiler-rt**, which is already part of this
monorepo at `compiler-rt/lib/builtins/`. The Makefile compiles the required
source files directly — no separate download or build step needed.

---

## Step 3 — Build the runtime programs

```bash
cd llvm/lib/Target/KlaussCPU/runtime
make all          # builds all .elf files
make hello        # single program
make clean        # remove build artifacts
```

Output is a set of `.elf` files. The FPGA loader reads ELF directly.

### Individual program example

```bash
make test_64bit.elf
```

### Adding a new program

1. Add `myprog.c` to `programs/`
2. Add a link rule to `Makefile` (copy any existing block)
3. Add `myprog` to the `PROGRAMS` list and the phony alias

---

## Step 4 — FreeRTOS port (optional)

A FreeRTOS V11.1.0 port runs on KlaussCPU using the existing timer interrupt
and MMIO interrupt mask — no new hardware needed.

```bash
cd llvm/lib/Target/KlaussCPU/runtime/freertos

# 1. Clone FreeRTOS-Kernel (once; shallow clone, ~2 MB).
bash get-freertos.sh

# 2. Build picolibc with -fPIC if you haven't already (see Step 2).

# 3. Build the demo.
make demo.elf            # 4-task smoke test, 7-seg shows CAFE
make console_demo.elf    # picolibc + UART line shell + lwIP telnet + wolfSSH
make net_demo.elf        # raw lwIP demo (no shell)
```

Output `.elf` files load through the same FPGA serial loader as the standalone
programs in Step 3. See [runtime/freertos/README.md](runtime/freertos/README.md)
for port internals (stack frame layout, `portYIELD` mechanics, kernel stack).

> **Network demos** also need lwIP and wolfSSL/wolfSSH. Fetch them first:
>
> ```bash
> bash ../get-lwip.sh                 # clones lwIP 2.2.0 into runtime/lwip/
> bash wolfssl/build-wolfssl.sh       # clones + builds wolfssl/wolfssl-{src,build,install}/
> bash wolfssl/build-wolfssh.sh       # clones + builds wolfssl/wolfssh-{src,build,install}/
> ```
>
> Our lwIP port (`sys_arch`, `ethernetif`, `cc.h`, `lwipopts.h`) lives in
> `lwip_port/` and `freertos/` and is in git; only the upstream lwIP core is
> fetched. `LWIP_CHKSUM_ALGORITHM=1` is set in `lwipopts.h` (the KlaussCPU
> byte-order checksum fix).

---

## Step 5 — Zephyr RTOS port (optional)

An out-of-tree Zephyr 3.7 LTS module boots `hello_world` end-to-end on hardware
(timer ISR, `printk` and `printf` via UART, main thread dispatch).

```bash
cd llvm/lib/Target/KlaussCPU/runtime/zephyr-ws

# 1. One-time setup: install west and pull Zephyr 3.7.
pip install --user west
west init -l klausscpu-zephyr
west update                          # clones zephyr/ into this directory

# 2. Build hello_world (paths assume LLVM built in Step 1).
LLVM_BIN=$PWD/../../../../../../build/bin
MOD=$PWD/klausscpu-zephyr
ZEPHYR=$PWD/zephyr

west build -b nexys_a7 zephyr/samples/hello_world --build-dir build_hello -- \
  -DZEPHYR_TOOLCHAIN_VARIANT=klausscpu-clang \
  -DKLAUSSCPU_LLVM_BIN=$LLVM_BIN \
  -DTOOLCHAIN_ROOT=$MOD \
  -DEXTRA_ZEPHYR_MODULES=$MOD \
  "-DARCH_ROOT=$MOD;$ZEPHYR" \
  "-DSOC_ROOT=$MOD;$ZEPHYR" \
  "-DBOARD_ROOT=$MOD;$ZEPHYR" \
  "-DDTS_ROOT=$MOD;$ZEPHYR"

# 3. Produce the loadable .kbt (the FPGA serial loader's input format).
cd build_hello/zephyr
klausscc -e zephyr.elf
# → build_hello/zephyr/zephyr.kbt
```

> `klausscc` is the FPGA serial loader, built from the separate Rust repo at
> `~/Documents/src/rust/klausscc/` (`cargo build --release` → `target/release/klausscc`).

See [runtime/zephyr-ws/klausscpu-zephyr/README.md](runtime/zephyr-ws/klausscpu-zephyr/README.md)
for the memory map, Kconfig constraints (`CONFIG_XIP=n`, no `-Os`, no debug
info), and the chain of fixes that made the port work.

### SSH shell + loadable programs (LLEXT) — `ssh_shell` app

Beyond `hello_world`, the `apps/ssh_shell` Zephyr app is the current top-level
system: it brings up Ethernet + lwIP + DHCP, serves an SSH login (wolfSSL/
wolfSSH), and exposes a `run` command that loads programs at runtime as Zephyr
LLEXT extensions (ELFCLASS32 `ET_REL` objects), each calling the kernel's
exported libc. This replaces the earlier custom PIC loader and is
hardware-confirmed, including concurrent `run` from multiple SSH sessions.

```bash
# Build the loadable extensions (from runtime/):
make ext-demos                       # hello/adventure/expr/bst/crypto/queens/test_64bit .llext
# copy the .llext files onto the SD card, then over SSH (pw: klausscpu):
#   ssh admin@<ip>
#   run adventure.llext

# Build the ssh_shell image (MUST include the wolfSSL + wolfSSH modules, or
# Kconfig aborts with "undefined symbol WOLFSSL"):
cd zephyr-ws
WOLFSSL=$PWD/klausscpu-zephyr/../../freertos/wolfssl/wolfssl-src
WOLFSSH=$PWD/klausscpu-zephyr/../../freertos/wolfssl/wolfssh-src
west build -b nexys_a7 klausscpu-zephyr/apps/ssh_shell --build-dir build_ssh -- \
  -DZEPHYR_TOOLCHAIN_VARIANT=klausscpu-clang -DKLAUSSCPU_LLVM_BIN=$LLVM_BIN \
  -DTOOLCHAIN_ROOT=$MOD -DEXTRA_ZEPHYR_MODULES="$MOD;$WOLFSSL;$WOLFSSH" \
  "-DARCH_ROOT=$MOD;$ZEPHYR" "-DSOC_ROOT=$MOD;$ZEPHYR" \
  "-DBOARD_ROOT=$MOD;$ZEPHYR" "-DDTS_ROOT=$MOD;$ZEPHYR"
```

The `zephyr/` upstream tree is fetched by `west update` and needs the vendored
LLEXT patch re-applied (`git -C zephyr apply
../klausscpu-zephyr/zephyr-patches/llext-klausscpu.patch`). The Zephyr README's
"Loadable extensions (LLEXT)" section documents the full mechanism, the patch
contents, and the build/config requirements.

---

## Quick smoke test (no FPGA needed)

Verify the compiler generates correct assembly:

```bash
cd build
printf 'define i64 @add(i64 %%a, i64 %%b) { %%r = add i64 %%a, %%b\n  ret i64 %%r\n}\n' \
  | ./bin/llc -march=klausscpu -O0 -x ir -
```

Expected output:
```
add:
    push    r15
    getsp   r15
    addr    r12, r0, r1
    setsp   r15
    pop     r15
    ret
```

Run the LLVM regression tests:

```bash
cd build
ninja llvm-readobj llvm-config
bin/llvm-lit ../llvm/test/CodeGen/KlaussCPU/
```

All 8 tests should pass.

---

## MMIO peripheral access

Peripherals are accessed via memory-mapped registers defined in `runtime/mmio.h`.
Include it in any program that uses LEDs, 7-segment, switches, or RGB:

```c
#include "../mmio.h"   /* from programs/ */
/* or */
#include "mmio.h"      /* from the runtime/ root */

REG_LEDS    = 0xAAAA;
REG_SEG_ALL = 0xDEAD;
seg7(0x1234);           /* inline wrapper — same as REG_SEG_ALL = 0x1234 */
leds(0x00FF);           /* inline wrapper */
uint32_t sw = switches();
```

UART is still via compiler intrinsics in `uart_stubs.c`; `printf`/`puts`
work via picolibc and go through the same UART path.

---

## What is and is not in git

| Item | In git? | Notes |
|------|---------|-------|
| LLVM backend source (`*.td`, `*.cpp`, `*.h`) | ✅ yes | |
| Runtime source (`src/`, `programs/`) | ✅ yes | |
| `mmio.h`, `klausscpu.ld`, `Makefile` | ✅ yes | |
| `build-picolibc.sh`, `get-lwip.sh`, `freertos/get-freertos.sh`, `freertos/wolfssl/build-wolfssl.sh`, `build-wolfssh.sh` | ✅ yes | run to fetch external sources |
| `compiler-rt/lib/builtins/` | ✅ yes | part of the LLVM monorepo |
| FreeRTOS port (`freertos/portable/`, `FreeRTOSConfig.h`, demos) | ✅ yes | |
| lwIP port (`lwip_port/`, `freertos/lwipopts.h`) | ✅ yes | upstream core fetched by `get-lwip.sh` |
| Zephyr port (`zephyr-ws/klausscpu-zephyr/`) + SSH/LLEXT app | ✅ yes | out-of-tree module |
| `picolibc-src/`, `picolibc-build/`, `picolibc-install/` | ❌ gitignored | cloned/built by `build-picolibc.sh` |
| `freertos/FreeRTOS-Kernel/` | ❌ gitignored | cloned by `get-freertos.sh` |
| `freertos/wolfssl/{wolfssl,wolfssh}-{src,build,install}/` | ❌ gitignored | cloned/built by `build-wolfssl.sh`/`build-wolfssh.sh` |
| `runtime/lwip/` (upstream lwIP 2.2.0 core) | ❌ gitignored | cloned by `get-lwip.sh` |
| `zephyr-ws/.west/`, `zephyr-ws/zephyr/`, `zephyr-ws/build*/` | ❌ gitignored | west workspace + Zephyr upstream + build outputs |
| `*.elf`, `*.bin`, `*.o`, `*.kbt` | ❌ gitignored | build outputs |
