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
│   └── programs/                  test and demo programs
│       ├── hello.c                UART smoke test
│       ├── adventure.c            text adventure game
│       ├── test_64bit.c           integer/memory/heap test suite
│       ├── expr.c                 recursive expression evaluator
│       ├── bst.c                  binary search tree + heap stress
│       ├── crypto.c               CRC32 / SHA-256 / Base64
│       ├── queens.c               N-queens backtracker
│       ├── test_switch.c          switch → BR_JT → JMPR_R
│       ├── test_fp.c              soft-FP smoke test
│       ├── test_asm.c             inline assembly test
│       └── test_printf.c          varargs / printf test
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
| `build-picolibc.sh` | ✅ yes | run it to fetch picolibc |
| `compiler-rt/lib/builtins/` | ✅ yes | part of the LLVM monorepo |
| `picolibc-src/` | ❌ gitignored | cloned by `build-picolibc.sh` |
| `picolibc-build/` | ❌ gitignored | meson build directory |
| `picolibc-install/` | ❌ gitignored | installed library |
| `*.elf`, `*.o` | ❌ gitignored | build outputs |
