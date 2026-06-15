# klauss-* crate workspace

Reusable `no_std` library crates for KlaussCPU Rust programs, factoring out
the boilerplate the standalone examples (`../hello-{smoke,uart,ext}`) each
duplicated. New programs depend on these instead of re-deriving MMIO
addresses, the console, the allocator, and the startup/panic glue.

## Crates

| Crate | Provides | Deps |
|---|---|---|
| `klauss-sys` | raw `extern "C"` decls (kernel exports + UART shim) | — |
| `klauss-mmio` | typed volatile peripherals: `leds`, `switches`, `seg_all`, `rgb*`, `clock_ms`, `read32/write32/read64` | — |
| `klauss-alloc` | `KernelHeap` `#[global_allocator]` over `malloc`/`free` (arbitrary alignment); `global_allocator!()` macro | sys |
| `klauss-io` | `println!`/`print!` + `Console: Write`; feature `uart` (shim) or `kernel` (loader-routed `putchar`) | sys |
| `klauss-rt` | bare-metal `entry!(main)` → `_start`/BSS/delay + `#[panic_handler]` | — |
| `klauss-llext-rt` | LLEXT `entry!(run)` → `extern "C" main`, kernel-heap allocator, panic handler | sys, alloc, io/kernel |

## Examples

| Example | Kind | Crates used | Build |
|---|---|---|---|
| `examples/blinky` | bare-metal ELF | mmio + rt + io/uart | `../klauss-build elf blinky` |
| `examples/ext-hello` | Zephyr `.llext` | llext-rt + io/kernel (alloc) | `../klauss-build llext ext-hello` |

## Building — use the `klauss-build` driver

[`../klauss-build`](../klauss-build) drives both flavors with one interface,
baking in the bits that are easy to get wrong by hand (the llext section-merge,
the weak-`mem*` strip, the loader invariant + import checks). Point it at the
stage1 rustc once (the only required external path); everything else is
discovered relative to the repo:

```sh
cp ../.klaussbuild.env.sample ../.klaussbuild.env   # then edit KLAUSSCPU_RUSTC
../klauss-build elf   blinky        # -> examples/blinky/blinky.elf + .bin
../klauss-build llext ext-hello     # -> examples/ext-hello/ext_hello.llext
../klauss-build all                 # both
```

`KLAUSSCPU_RUNTIME` (optional) makes the llext build cross-check every import
against the kernel's `ssh/llext_exports.c`, catching a missing export on the
host instead of as a load failure on the board.

### Why per-package, and the two flavors

The `klauss-io` sink features `uart` and `kernel` are **mutually exclusive**
(a program has one console), so the driver builds one package at a time — a
whole-workspace `cargo build` would unify `klauss-io` to both and trip the
`compile_error!` guard (which also catches a program that forgot to pick a
sink). The flavors differ in `build-std-features`: bare-metal keeps
`compiler-builtins-mem` (binaries link standalone), the llext drops it so
`mem*` resolve against the kernel — the driver handles both. `.cargo/config.toml`
holds only machine-independent settings; the driver injects the linker paths.

## Migration note

The three standalone `../hello-*` crates predate this workspace and remain as
hardware-confirmed reference points. New work should use these crates; the
standalone examples can be retired once `blinky`/`ext-hello` are
board-confirmed. The natural home for this workspace long-term is the
`klausscpu-runtime` repo (RUST_PLAN.md §5).
