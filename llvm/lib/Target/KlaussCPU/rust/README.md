# Rust on KlaussCPU — rebuilding the toolchain from a fresh clone

Everything required to stand the Rust environment back up lives in this
directory:

| Path | What it is |
|---|---|
| `klausscpu-unknown-none-elf.json` | rustc target spec (data layout must byte-match `KlaussCPUTargetMachine.cpp`) |
| `rustc-patches/0001-*.patch`, `0002-*.patch` | the complete rustc fork delta (callconv module, `rustc_llvm` wiring, llvm-wrapper guard fixes) |
| `rustc-patches/bootstrap.toml` | rustc bootstrap config template (edit the `llvm-config` path) |
| `hello-smoke/` | bare-metal MMIO smoke test (M1 artifact; no C runtime at all) |
| `hello-uart/` | bare-metal UART hello via `core::fmt` + C shim (M2 artifact) |
| `hello-ext/` | Zephyr LLEXT extension with `alloc` over the kernel heap (M3 artifact); `build-llext.sh` is the packaging pipeline |
| `crates/` | the `klauss-*` reusable library crates (a Cargo workspace) + `blinky`/`ext-hello` examples built from them — see `crates/README.md` |
| `klauss-build` | one driver for both flavors (`elf` / `llext`), with path discovery, section-merge, and loader/import verification baked in |
| `klausscpu.ld` | bundled bare-metal linker script (mirror of the runtime's; makes bare-metal links self-contained) |
| `.klaussbuild.env.sample` | template for the machine-local `.klaussbuild.env` (gitignored) the driver sources |

Pinned versions (re-verify on any change — see "Rebasing" below):

| Component | Pin |
|---|---|
| this repo (LLVM fork) | LLVM 23.0.0git, `KlaussCPU` experimental target |
| rust-lang/rust | commit `09a371361240e42b0d69438fd1179efcf212e576` (master, 1.98.0-dev, bundles LLVM 22.1) |
| host cargo | any rustup nightly ≥ 1.95 (only used to drive builds; supplies `-Zbuild-std`/`-Zjson-target-spec`) |

Host prerequisites (macOS): `cmake`, `ninja`, `ccache` (optional), `python3`,
rustup with a nightly toolchain, and **zstd** (`brew install zstd`) — the
fork's LLVM links against it.

---

## 1. Build the LLVM fork

```sh
git clone <this-repo> klausscpu-llvm && cd klausscpu-llvm
cmake -G Ninja -S llvm -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_TARGETS_TO_BUILD="X86" \
  -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD="KlaussCPU" \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_CCACHE_BUILD=ON

# Tools + libs. rustc's bootstrap needs MORE than the board builds do:
ninja -C build clang lld llvm-config llvm-libraries FileCheck \
  llvm-cov llvm-nm llvm-objcopy llvm-objdump llvm-profdata llvm-readelf \
  llvm-readobj llvm-size llvm-strip llvm-ar llvm-as llvm-dis llvm-link \
  llc opt llvm-lit count not
```

`llvm-libraries` (every component archive) and `FileCheck` + the
`llvm-cov … opt` tool list are hard requirements of rustc's sanity checks —
their absence fails the rustc build at minute 3 with confusing errors.

> **Path note:** the checked-in `.cargo/config.toml` files and
> `build-llext.sh` default to the toolchain at
> `~/Documents/src/klausscpu-llvm/build/bin`. On a different machine, adjust
> the paths / set `KLAUSSCPU_LLVM_BIN`. They also reference
> `~/Documents/src/klausscpu-runtime/klausscpu.ld` for bare-metal links.

## 2. Build the patched rustc (stage1)

```sh
git clone --depth 1 https://github.com/rust-lang/rust.git klausscpu-rust
cd klausscpu-rust
git fetch --depth 1 origin 09a371361240e42b0d69438fd1179efcf212e576
git checkout 09a371361240e42b0d69438fd1179efcf212e576 -b klausscpu
git am <this-repo>/llvm/lib/Target/KlaussCPU/rust/rustc-patches/00*.patch

cp <this-repo>/llvm/lib/Target/KlaussCPU/rust/rustc-patches/bootstrap.toml .
#   → edit [target.*] llvm-config to point at YOUR fork build/bin/llvm-config

LIBRARY_PATH=/usr/local/lib ./x.py build --stage 1 library
#   LIBRARY_PATH: homebrew zstd for the rustc_driver link (Apple Silicon:
#   /opt/homebrew/lib). ~25 min on 8 cores. Result:
#   build/<host>/stage1/bin/rustc   (1.98.0-dev)
```

The stage1 sysroot contains a `rust-src` link automatically — that is what
`-Zbuild-std` compiles `core`/`alloc` from.

## 3. Build the example crates

**Preferred path — the `klauss-build` driver** ([`klauss-build`](klauss-build)).
It builds both flavors with one interface, discovering toolchain paths relative
to the repo and baking in the steps that are easy to get wrong by hand (the
llext section-merge, weak-`mem*` strip, loader invariant + import checks). Set
the one required external path once:

```sh
cp .klaussbuild.env.sample .klaussbuild.env   # edit KLAUSSCPU_RUSTC (stage1 rustc)
./klauss-build elf   blinky        # -> crates/examples/blinky/blinky.elf + .bin
./klauss-build llext ext-hello     # -> crates/examples/ext-hello/ext_hello.llext
./klauss-build all                 # both
```

It operates on the `crates/` workspace; see `crates/README.md`. Setting
`KLAUSSCPU_RUNTIME` additionally cross-checks every llext import against the
kernel's `ssh/llext_exports.c`.

**Standalone `hello-*` crates** (legacy reference, predate the workspace) build
directly. Bare metal → a normal ELF for the `klausscc` serial loader:

```sh
export RUSTC=<klausscpu-rust>/build/x86_64-apple-darwin/stage1/bin/rustc
cd hello-uart && cargo +nightly build --release
cp target/klausscpu-unknown-none-elf/release/hello-uart hello-uart.elf
```

LLEXT extension → `cd hello-ext && ./build-llext.sh` (→ `hello_rs.llext`); the
script self-verifies the loader invariants and now also does the section-merge
(see the gotcha below).

## 4. Gotchas (each cost real debugging time)

- **JSON targets are double-gated**: `-Zunstable-options` in `rustflags`
  (rustc side) *and* `json-target-spec = true` under `[unstable]` (cargo
  side). Both are in the checked-in `.cargo/config.toml`s.
- **Backend changes need a rustc relink**: stage1 rustc links LLVM
  statically. After touching the KlaussCPU backend: `ninja -C build llc` to
  test, then re-run the `x.py` command (≈1–15 min, mostly cached) before
  Rust builds see the change.
- **LLEXT size control lives in three places** (`hello-ext` is the
  reference): per-package `codegen-units = 256` for `compiler_builtins`
  (cargo's "did not match any packages" warning is wrong — it works);
  `build-llext.sh` strips the weak `memcpy`/`memset` builtins members so
  mem* resolve against the kernel exports; and `ld.lld -r -u main` against
  the archive (member selection — `-r` cannot gc-sections). Skipping any of
  these turns 31 KB into 1.3 MB.
- **Rust `.llext` must merge per-function sections** (hardware-confirmed
  2026-06-15). Rust defaults to function-sections → 100+ `.text.<mangled>`
  sections; the KlaussCPU LLEXT loader only handles a C-like single-`.text`
  layout and mis-relocates internal cross-section calls (target resolves to
  `0` → `PC=0` crash *after* the first print, which only used the external
  `putchar`). The fix is the `-T crates/llext-merge.ld` step in the
  `build-llext.sh` scripts — it collapses everything to `.text`/`.rodata`.
  Crash signature: `PC=0`/`RA=0`, dies right after the first external-only
  output line.
- **`lib.rmeta is neither ET_REL nor LLVM bitcode`** warnings from ld.lld
  are benign (metadata members in rlibs).
- **Extension panics park the SSH connection thread** (no `llext_exit`
  kernel export yet — RUST_PLAN.md §6.5).

## 5. Rebasing the pins

The fork tracks LLVM trunk; rustc master moves its LLVM API expectations.
When bumping either side, re-audit `compiler/rustc_llvm/llvm-wrapper/` —
patch `0002` exists precisely because rust master's `LLVM_VERSION_GE(23,0)`
guards were written for a *newer* 23-trunk snapshot than this fork
(`getMCSubtargetInfo()` pointer vs reference, `CfiFunctionIndex` API,
`Os/Oz` opt levels). Record the new (fork commit ↔ rust commit) pair here
and regenerate `rustc-patches/` via:

```sh
git -C <klausscpu-rust> format-patch <base>..klausscpu \
    -o <this-repo>/llvm/lib/Target/KlaussCPU/rust/rustc-patches/
```
