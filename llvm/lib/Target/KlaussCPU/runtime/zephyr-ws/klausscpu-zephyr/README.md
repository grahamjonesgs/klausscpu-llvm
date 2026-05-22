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
klausscc -e zephyr.elf -c /path/to/runtime/opcode_select_opcodes.json
# → zephyr.kbt
```

## Loading and monitoring

```sh
klausscc -e zephyr.elf -c .../opcode_select_opcodes.json \
         --serial /dev/tty.usbserial-... --monitor
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
- **Optimisation**: `-O2` only. `-Os` and debug info both trip backend bugs.
- **`CONFIG_CBPRINTF_FP_SUPPORT=n`**: pulling double-precision FP into cbprintf
  forces soft-FP imports the runtime doesn't currently provide.
- **`arch_printk_char_out` is a strong override** in `arch/klausscpu/core/irq.c`,
  plus a `SYS_INIT` installs the same function as the libc stdout hook. The
  upstream `drivers/console/uart_console.c` isn't compiled in this build, so
  without these overrides `printk` and `printf` would silently discard output.
- **`OUTPUT_FORMAT("elf32-klausscpu")`** entry has been added to lld's
  `ScriptParser.cpp` `StringSwitch` table (in the LLVM tree, not here).
