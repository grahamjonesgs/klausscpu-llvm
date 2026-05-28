# KlaussCPU Zephyr port

Out-of-tree Zephyr 3.7 LTS port for the KlaussCPU 64-bit FPGA core on the
Digilent Nexys A7. Provides arch, SoC, board, drivers (UART + system timer),
linker script, DTS bindings and toolchain integration for `clang -target klausscpu`.

## Status — `hello_world` runs end-to-end

```
*** Booting Zephyr OS build v3.7.0 ***
Hello World! nexys_a7
```

Boots the kernel, runs the timer ISR, dispatches the main thread, and emits
`printk`/`printf` output to the UART.

## Layout

| Path | Purpose |
|---|---|
| `arch/klausscpu/core/swap.S` | `__start`, `arch_switch`, timer ISR (`z_klausscpu_timer_isr`) |
| `arch/klausscpu/core/thread.c` | `arch_new_thread` (initial stack frame) |
| `arch/klausscpu/core/irq.c` | IRQ enable/disable, kernel stack pointer, `arch_printk_char_out` + stdout hook |
| `drivers/timer/klausscpu_timer.c` | Zephyr sys_clock driver (MMIO timer at `0xF00F_0000`) |
| `drivers/serial/uart_klausscpu.c` | Polled UART driver using the `txcharmemr` builtin |
| `soc/klausscpu/nexys_a7/linker.ld` | Memory layout — ROM at `0x20`, RAM at `0x00100000`, stack at top |
| `boards/klausscpu/nexys_a7/nexys_a7.dts` | DTS: console UART, system timer |
| `boards/klausscpu/nexys_a7/nexys_a7_defconfig` | Per-board Kconfig defaults (XIP=n, minimal libc, UART_CONSOLE, …) |
| `include/zephyr/arch/klausscpu/arch.h` | Inline `arch_irq_lock`/`unlock`, cycle counter access |

## One-time setup

```sh
# From the parent directory (zephyr-ws):
pip install --user west
west init -l klausscpu-zephyr        # if not already initialised
west update                          # pulls Zephyr 3.7 into ./zephyr/
```

After `west update` the workspace looks like:
```
zephyr-ws/
├── .west/                  ← west state (gitignored)
├── zephyr/                 ← Zephyr 3.7 LTS (gitignored, fetched by west)
└── klausscpu-zephyr/       ← this module (tracked in git)
```

## Building

The build needs an LLVM tree containing the KlaussCPU backend. Build LLVM/clang
first (see the parent `llvm-project` README), then:

```sh
cd zephyr-ws

LLVM_BIN=/path/to/llvm-project/build/bin
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
```

Incremental rebuilds:
```sh
ninja -C build_hello
```

## Producing the loadable `.kbt`

The board's serial loader consumes a `.kbt` file (custom ASCII wire format
produced by `klausscc`). After the ELF is linked, run:

```sh
cd build_hello/zephyr
klausscc -e zephyr.elf
# → zephyr.kbt
```

`klausscc` is the FPGA serial loader / assembler, built from the separate Rust
repo at `~/Documents/src/rust/klausscc/` (`cargo build --release` → `target/release/klausscc`).

## Loading and monitoring

```sh
klausscc -e zephyr.elf --serial /dev/tty.usbserial-... --monitor
```

The `--monitor` flag tails the UART after the load finishes; expect:
```
Load Complete OK
*** Booting Zephyr OS build v3.7.0 ***
Hello World! nexys_a7
```

## Memory map (post-link)

```
0x00000000–0x0000001F  hardware heap header (8 bytes used)
0x00000020–0x000FFFFF  ROM   (text + rodata + initlevel + ksort tables)
0x00100000–0x07EFFFFF  RAM   (data + bss + .noinit + thread stacks + heap)
0x07F00000–0x07FFFFFF  Boot/exception stack (1 MB at top of 128 MB RAM)
```

The RAM origin must stay within `klausscc`'s `MAX_SEGMENT_GAP` (1 MB) of the
ROM end, otherwise `parse_elf_to_flat` silently drops the RW segment and the
kernel crashes accessing uninitialised globals. Keep ROM ≤ 1 MB or move RAM
origin to match.

## Notable workarounds and constraints

- **`CONFIG_XIP=n`**: the loader (`klausscc`) only handles PT_LOAD segments
  with `VirtAddr == PhysAddr`. XIP would put `.data` LMA in ROM with a different
  PhysAddr — the loader would skip it. With XIP=n the linker collapses LMA into
  VMA and `__start`'s copy loop becomes a no-op (src == dst).
- **Optimisation / debug info**: defaults are `-O2` and `CONFIG_DEBUG_INFO=n`.
  The backend handles `-g`, `-Os`, and `-Os -g` correctly; debug info is left
  off only to keep `zephyr.elf` small (it roughly doubles in size). Pass
  `-DCONFIG_DEBUG_INFO=y` to `west build` for source-level debugging.
- **`CONFIG_CBPRINTF_FP_SUPPORT=n`**: pulling double-precision FP into cbprintf
  forces soft-FP imports the runtime doesn't currently provide.
- **`arch_printk_char_out` is a strong override** in `arch/klausscpu/core/irq.c`,
  plus a `SYS_INIT` installs the same function as the libc stdout hook. The
  upstream `drivers/console/uart_console.c` isn't compiled in this build, so
  without these overrides `printk` and `printf` would silently discard output.
- **`OUTPUT_FORMAT("elf32-klausscpu")`** entry has been added to lld's
  `ScriptParser.cpp` `StringSwitch` table (in the LLVM tree, not here).

## Loadable extensions (LLEXT) — the SSH `run` command

Status: **working on hardware.** The SSH shell's `run` command loads programs at
runtime as Zephyr LLEXT extensions (ELFCLASS32 `ET_REL` objects), replacing the
earlier custom PIC loader. Each program is a plain `.o` that calls the kernel's
exported libc; the loader resolves those symbols, relocates the code into the
llext heap, and runs the program's `main()` with stdio redirected to the SSH
session.

### Building an extension

From `runtime/`:

```sh
make ext-demos      # builds hello/adventure/expr/bst/crypto/queens/test_64bit .llext
```

Extensions compile with `-c` against the SDK headers in `runtime/ext_include/`
(plain externs — picolibc headers can't be used because they macro-define
`putchar`/`getchar` over `FILE*`). To add a program, drop it in `runtime/programs/`
and add its name to `EXT_DEMOS` in the Makefile. It may call any symbol exported
in `ssh/llext_exports.c` (printf, puts, putchar, getchar, malloc/calloc/realloc/
free, mem*/str*) plus the inline MMIO helpers in `mmio.h`.

### Running it

Copy the `.llext` to the SD card, then over SSH (`ssh admin@<ip>`, pw `klausscpu`):

```
run adventure.llext          # or: run /SD:/adventure.llext
```

The program's `printf`/`puts`/`putchar` output and `getchar` input flow over the
SSH session; on exit the extension is unloaded.

### How it works (key files)

| File | Role |
|---|---|
| `ssh/llext_loader.c` | reads the `.llext` from SD, `llext_load`, resolves `main`, runs it on a thread, redirects I/O |
| `ssh/llext_exports.c` | `EXPORT_SYMBOL` table (kernel libc) + `getchar()` (minimal libc has none) |
| `arch/klausscpu/core/elf.c` | `arch_elf_relocate` — ABS32 / ABS64 / PCREL32, little-endian |
| `arch/klausscpu/core/irq.c` | `arch_printk_char_out` console-output redirect hook |
| `runtime/ext_include/` | extension SDK headers (stdio/string/stdlib/stdint/stddef) |

### Build / config requirements

- `apps/ssh_shell/prj.conf` enables `CONFIG_LLEXT`, `CONFIG_LLEXT_TYPE_ELF_OBJECT`,
  a `CONFIG_LLEXT_HEAP_SIZE`, and **`CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE`** (minimal
  libc defaults the malloc arena to 0, i.e. `malloc()` returns NULL). It also sets
  `CONFIG_LLEXT_LOG_LEVEL_WRN` to silence the per-relocation load trace.
- The `west build` for `ssh_shell` **must include the wolfSSL and wolfSSH modules**
  or Kconfig aborts (`undefined symbol WOLFSSL`):
  `-DEXTRA_ZEPHYR_MODULES="$MOD;$WOLFSSL;$WOLFSSH"` where `$WOLFSSL` /
  `$WOLFSSH` point at `runtime/freertos/wolfssl/{wolfssl-src,wolfssh-src}`.
- **Vendored Zephyr patches** (the `zephyr/` tree is git-ignored, fetched by west).
  Captured in `zephyr-patches/llext-klausscpu.patch`; re-apply after `west update`:
  ```sh
  git -C zephyr apply ../klausscpu-zephyr/zephyr-patches/llext-klausscpu.patch
  ```
  The patch: `subsys/llext/Kconfig` adds `LLEXT_ELF_CLASS32`;
  `include/zephyr/llext/elf.h` parses ELF32 under `CONFIG_64BIT` when that is set;
  `subsys/llext/llext_link.c` accepts `SHT_RELA` on the generic path;
  `subsys/llext/llext_load.c` recovers the merged LLVM string table, treats an
  identical SHSTRTAB/STRTAB alias as non-overlapping, and honours a new
  `keep_symtab` load-param flag (`include/zephyr/llext/llext.h`) so `main` can be
  resolved by name after load.
