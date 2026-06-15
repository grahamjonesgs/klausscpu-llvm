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
| `examples/blinky` | bare-metal ELF | mmio + rt + io/uart | `cargo build -p blinky --release` → `target/.../blinky` |
| `examples/ext-hello` | Zephyr `.llext` | llext-rt + io/kernel (alloc) | `examples/ext-hello/build-llext.sh` → `ext_hello.llext` |

Set `RUSTC` to the stage1 compiler first:

```sh
export RUSTC=<klausscpu-rust>/build/x86_64-apple-darwin/stage1/bin/rustc
```

## Build one example at a time

The `klauss-io` sink features `uart` and `kernel` are **mutually exclusive**
(a program has one console). So build examples *individually* with `-p`:
a whole-workspace `cargo build` would unify `klauss-io` to both features and
trip the `compile_error!` guard. That guard is deliberate — it also catches a
program that forgets to pick a sink.

`build.target`, `build-std`, and the linker flags live in
`.cargo/config.toml` (shared). The bare-metal default keeps
`compiler-builtins-mem` (binaries link standalone); `ext-hello`'s build script
overrides it with `-Z build-std-features=optimize_for_size` so `mem*` resolve
against the kernel exports. Toolchain paths default to the machine-local
`build/bin`; override with `KLAUSSCPU_LLVM_BIN`. See `../README.md` for the
full rebuild-from-clone guide.

## Migration note

The three standalone `../hello-*` crates predate this workspace and remain as
hardware-confirmed reference points. New work should use these crates; the
standalone examples can be retired once `blinky`/`ext-hello` are
board-confirmed. The natural home for this workspace long-term is the
`klausscpu-runtime` repo (RUST_PLAN.md §5).
