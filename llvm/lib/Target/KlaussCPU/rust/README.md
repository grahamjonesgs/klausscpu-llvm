# Rust on KlaussCPU — rebuilding the toolchain from a fresh clone

Everything required to stand the Rust environment back up lives in this
directory:

| Path | What it is |
|---|---|
| `klausscpu-unknown-none-elf.json` | rustc target spec (data layout must byte-match `llvm/lib/TargetParser/TargetDataLayout.cpp`; `llvm/test/CodeGen/KlaussCPU/rust-target-spec.test` checks it) |
| `rustc-patches/0001-*.patch` … `0003-*.patch` | the complete rustc fork delta (callconv module, `rustc_llvm` wiring, llvm-wrapper guard fixes; 0003: C ABI version 2 and `va_arg`) |
| `rustc-patches/bootstrap.toml` | rustc bootstrap config template (edit the `llvm-config` path) |
| `hello-smoke/` | bare-metal MMIO smoke test (M1 artifact; no C runtime at all) |
| `hello-uart/` | bare-metal UART hello via `core::fmt` + C shim (M2 artifact) |
| `hello-ext/` | Zephyr LLEXT extension with `alloc` over the kernel heap (M3 artifact); `build-llext.sh` is the packaging pipeline |
| `crates/` | the `klauss-*` reusable library crates (a Cargo workspace) + `blinky`/`ext-hello`/`ext-atomics`/`ffi-check` examples built from them — see `crates/README.md` |
| `abi-test/` | Rust <-> C ABI conformance test (generated callers/callees in both languages against host gcc, on the emulator) — see "Checking the ABI" |
| `klauss-build` | one driver for both flavors (`elf` / `llext`), with path discovery, section-merge, and loader/import verification baked in |
| `klausscpu.ld` | bundled bare-metal linker script (mirror of the runtime's, which klausscpu-runtime's `check-toolchain-constants.py` compares; makes bare-metal links self-contained) |
| `.klaussbuild.env.sample` | template for the machine-local `.klaussbuild.env` (gitignored) the driver sources |
| `klausscpu-unknown-linux-musl.json` | the Linux target as a JSON spec, for a rustc without patch 0005 (which builds it in; rustc checks a built-in data layout against LLVM itself); the lit test checks the JSON |
| `rustc-patches/0004-*.patch`, `rustc-patches/backtrace/0001-*.patch` | `std`/`unwind` for that target, and backtrace-rs reading ELF32 (part 7) |
| `rustc-patches/0005`, `0006`, `cargo/`, `bootstrap-cross.toml` | the built-in Linux target, rustc and cargo built to run on the board (part 8) |
| `libc-patches/`, `crate-patches/`, `setup-crate-forks.sh` | the crates.io crates with a `klausscpu` port (`libc` for std; `cc`, `linux-raw-sys`, `rustix` for rustc and cargo), as patches; the script makes the forks in a rust checkout |
| `klauss-linux-cargo`, `klauss-linux-run` | cargo for the Linux target, and the runner that runs its programs on the board over SSH (part 7) |
| `linux-std/` | `stdcheck` (a std smoke test), `library-tests.sh` (Rust's own core/alloc/std test suites on the board) and `libc-test/` (the `libc` fork against musl's headers) |

Pinned versions (re-verify on any change — see "Rebasing" below):

| Component | Pin |
|---|---|
| this repo (LLVM fork) | LLVM 23.0.0git, `KlaussCPU` experimental target |
| rust-lang/rust | commit `09a371361240e42b0d69438fd1179efcf212e576` (master, 1.98.0-dev, bundles LLVM 22.1) |
| host cargo | any rustup nightly ≥ 1.95, or any recent stable cargo with `RUSTC_BOOTSTRAP=1` (only used to drive builds; supplies `-Zbuild-std`/`-Zjson-target-spec`; `klauss-build` falls back to the stable one by itself) |

> **C ABI version 3 (`../ABI.md`, section "Rust"; 3 is 2 with `i128`/`u128` 8-aligned, which rustc takes from the target spec's data layout).**  Patch `0003` makes
> the callconv module (`callconv/klausscpu.rs`) classify like clang: `u32`
> sign-extended, aggregates of up to 16 bytes in one or two GPRs, larger ones
> by reference / `sret`.  A rustc built from `0001`–`0002` alone still
> implements version 0 and disagrees with C on `u32` and on structs by value;
> rebuild it.  `abi-test/` checks the agreement.

Host prerequisites (macOS): `cmake`, `ninja`, `ccache` (optional), `python3`,
rustup with a nightly toolchain, and **zstd** (`brew install zstd`) — the
fork's LLVM links against it.  Linux (x86-64, phase 5): the same tools, the
distribution's `libzstd`/`zlib` development files, and any rustup toolchain
(stable is enough; see the cargo pin above).

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

# 0004 and 0006 take libc (std, rustc), cc (bootstrap), linux-raw-sys and
# rustix from forks inside the checkout, so they must exist before x.py:
# crates.io + libc-patches/ and crate-patches/, excluded from git.
<this-repo>/llvm/lib/Target/KlaussCPU/rust/setup-crate-forks.sh .

cp <this-repo>/llvm/lib/Target/KlaussCPU/rust/rustc-patches/bootstrap.toml .
#   → edit [target.<host triple>] (x86_64-apple-darwin, x86_64-unknown-linux-gnu,
#     ...) and its llvm-config to point at YOUR fork build/bin/llvm-config

LIBRARY_PATH=/usr/local/lib ./x.py build --stage 1 library
#   LIBRARY_PATH: homebrew zstd for the rustc_driver link (Apple Silicon:
#   /opt/homebrew/lib; not needed on Linux). ~25 min on 8 cores, 5 min on
#   24 (Linux, the LLVM fork already built). Result:
#   build/<host>/stage1/bin/rustc   (1.98.0-dev)
```

The stage1 sysroot contains a `rust-src` link automatically — that is what
`-Zbuild-std` compiles `core`/`alloc` from.

Then, for `std` on Linux (part 7), patch the backtrace submodule **without
committing** (x.py resets a submodule whose checked-out commit differs from
the recorded one, but keeps uncommitted changes):

```sh
git -C library/backtrace apply \
  <this-repo>/llvm/lib/Target/KlaussCPU/rust/rustc-patches/backtrace/0001-*.patch
```

`bootstrap.toml` has `deny-warnings = false`: the `libc` fork, a path
dependency of the library workspace, has warnings of its own that bootstrap
would otherwise turn into errors.

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
./klauss-build llext ext-atomics   # -> crates/examples/ext-atomics/ext_atomics.llext
./klauss-build all                 # all of them
```

It operates on the `crates/` workspace; see `crates/README.md`. Setting
`KLAUSSCPU_RUNTIME` additionally cross-checks every llext import against the
kernel's `ssh/llext_exports.c`.  `KLAUSSCPU_WS` points it at another
workspace instead: klausscpu-runtime's `rust/` (programs for the board,
built, copied and run by its `tools/klauss-rust`) uses this.

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

## 4. Atomics

The target spec has `max-atomic-width` 64 and `atomic-cas` true, so
`core::sync::atomic` has every type up to `AtomicU64`/`AtomicUsize` with the
full API, and `alloc::sync::Arc` exists.  The ISA has no atomic
instructions; the backend's "forced atomics" model (`../CLAUDE.md`, row
"Atomics") makes an aligned atomic load or store a plain access, a fence a
compiler barrier, and every read-modify-write and compare-exchange a call to
`__sync_*_N` (1/2/4/8 bytes).  Those run between a `DI` (which returns the old
interrupt-enable bit) and an `EI` that puts it back, about 3 cycles
(`INT_MASK`, the per-source mask, is not touched): **atomic against this
core's interrupt handlers, not against the second (AMP) core**.  rustc has no
`asm!` for this target, so `klauss-rt` calls the backend's
`llvm.klausscpu.di` / `.ei` intrinsics through `link_llvm_intrinsics`; a
rustc whose private LLVM predates them (before the relink in the part above)
cannot build `klauss-rt`'s `sync-libcalls` feature.  Who provides them:

- a bare-metal Rust program: `klauss-rt` (default feature `sync-libcalls`,
  `crates/klauss-rt/src/sync.rs`, the twin of the C runtime's `atomic.c`);
  turn the feature off when the program also links `atomic.c`;
- a Rust staticlib in a C program: the C runtime's `src/atomic.c`;
- an LLEXT extension: the Zephyr kernel's own, from the runtime's
  `atomic.c`, which `ssh/llext_exports.c` exports (`EXPORT_SYNC_ALL(N)` for
  N = 1, 2, 4, 8; `klauss-build llext` accepts these imports).
  `crates/examples/ext-atomics` uses every one of them and checks the
  results (`run ext_atomics.llext` on the board: 63 pass, 0 fail).

The spec no longer sets `singlethread`: it only reaches LLVM's thread model,
which no KlaussCPU code reads, and it would claim that nothing interrupts
the program.  `llvm/test/CodeGen/KlaussCPU/rust-target-spec.test` checks that
the spec's widths are exactly what the backend lowers without `__atomic_*`
calls.  `crates/examples/ffi-check` checks that each `__sync_*` call puts a
non-zero `INT_MASK` and the interrupt-enable bit back as it found them, and races the main loop against a
timer interrupt on counters of every width (as the C runtime's
`test_atomic`): the atomic counters come out exact while a plain one loses
updates.

## 5. Checking the ABI

```sh
KLAUSSCPU_RUSTC=<stage1 rustc> KLAUSSCPU_LLVM_BIN=<repo>/build/bin \
  abi-test/run_rust_abi_test.sh 1 20 80      # seeds 1-20, 80 functions each
```

For each seed `gen_rust_abi_test.py` writes a caller and a callee in C and in
Rust (integers of every width, `bool`, pointers, `f32`/`f64`, `i128`,
`#[repr(C)]` structs and unions of 1–32 bytes with fields of every integer
width up to `i128`/`u128`, `bool` and floats, variadic functions); host gcc
gives the expected output, and C→Rust, Rust→C and Rust→Rust are run on the
klausscc emulator at every mix of C -O0/-O1/-O2/-Os and Rust opt-level
0/1/2/s (48 runs per seed).  It needs the runtime checkout (crt0, picolibc;
`KLAUSSCPU_RUNTIME`) and builds `core` once with cargo `-Zbuild-std`.
`crates/examples/ffi-check` is the hand-written counterpart for the board
(`./klauss-build elf ffi-check`, then load `ffi_check.elf`): structs both
ways, `u32` arguments and results, atomics of every width shared with C,
`INT_MASK` restored, a timer-interrupt race; it prints `Result: PASS` and
halts.

## 6. Gotchas (each cost real debugging time)

- **JSON targets are double-gated**: `-Zunstable-options` in `rustflags`
  (rustc side) *and* `json-target-spec = true` under `[unstable]` (cargo
  side). Both are in the checked-in `.cargo/config.toml`s.
- **Backend changes need a rustc relink**: stage1 rustc links LLVM
  statically. After touching the KlaussCPU backend: `ninja -C build llc` to
  test, rebuild the LLVM that bootstrap.toml's `llvm-config` names, then
  re-run the `x.py` command (≈1–15 min, mostly cached) before Rust builds
  see the change.  Neither step notices the change on its own: x.py does
  not track an external LLVM's archives (`touch compiler/rustc_llvm/build.rs`
  first, so it rebuilds `rustc_llvm` and relinks `rustc_driver`), and cargo
  keys its cache on `rustc -vV`, which a relink leaves the same (delete the
  crate's `target/` before rebuilding an extension, or it is not compiled
  again).
- **LLEXT size control lives in three places** (`hello-ext` is the
  reference): per-package `codegen-units = 256` for `compiler_builtins`
  (cargo's "did not match any packages" warning is wrong — it works);
  `build-llext.sh` strips the weak `memcpy`/`memset` builtins members so
  mem* resolve against the kernel exports; and `ld.lld -r -u main` against
  the archive (member selection — `-r` cannot gc-sections). Skipping any of
  these turns 31 KB into 1.3 MB.
- **LLEXT code needs long calls**: the 1-word short CALL reaches +-512 KiB
  and lld bridges further calls with thunks, which the LLEXT loader cannot
  add.  `klauss-build llext` and `hello-ext/.cargo/config.toml` pass
  `-Cllvm-args=-klausscpu-short-calls=false`; `verify_llext` rejects an
  `R_KLAUSSCPU_CALL18` that slips through.
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
  are benign (metadata members in rlibs).  Since patch 0005 rustc wraps
  them in KlaussCPU ELF objects and the warnings are gone.
- **Extension panics park the SSH connection thread** (no `llext_exit`
  kernel export yet — RUST_PLAN.md §6.5).

## 7. std on Linux

Programs for the board's Linux (klausscpu-runtime `LINUX_PLAN.md`) get the
whole standard library: threads, files, processes, pipes, TCP/UDP and DNS,
time, thread-locals, atomics, unwinding panics, backtraces with function
names.  The target is `klausscpu-unknown-linux-musl` (the same LLVM triple as
the C toolchain's; built into rustc by patch 0005, the JSON file of that name
before it): LP64 in ELFCLASS32 files, musl 1.2.5, LLVM's libunwind,
`panic = unwind`, PIC code and real thread-locals (the JSON spec had the
static relocation model and pthread-key thread-locals).

```sh
cd any-cargo-project
<this-dir>/klauss-linux-cargo build --release   # target/klausscpu-unknown-linux-musl/release/
<this-dir>/klauss-linux-cargo run --release     # ... and run it on the board
<this-dir>/klauss-linux-cargo test --release    # the tests, on the board
```

- **`klauss-linux-cargo`** passes everything on cargo's command line, so a
  project needs no `.cargo/config.toml`: `-Zbuild-std=std,panic_unwind,test`
  from the patched rustc's `rust-src`, the target (the JSON file for a rustc
  without patch 0005), clang as the linker
  with the musl + libc++ sysroot (`KLAUSSCPU_LINUX_SYSROOT`, default the
  runtime's `linux-ws/dyn/sysroot`), and `klauss-linux-run` as the runner.
  It reads `KLAUSSCPU_RUSTC` from `.klaussbuild.env` as `klauss-build` does.
- **Static or dynamic.** Programs are static by default.
  `KLAUSSCPU_LINUX_DYNAMIC=1` links them against the board's `libc.so` and
  `libunwind.so.1` instead (`-C target-feature=-crt-static`); that makes a
  PIE, because the clang driver only gives a program an interpreter with `-pie`
  (as for C: the board has no non-PIE dynamic programs).  Use a separate
  `--target-dir` for each: cargo does not rebuild std when only these flags
  change.
- **`klauss-linux-run <program> [args]`** copies the program to
  `/data/rust-run` on the board (`KLAUSSCPU_BOARD`, `KLAUSSCPU_BOARD_KEY`,
  `KLAUSSCPU_BOARD_DIR`) and runs it over SSH with its exit status, holding
  both board locks (as `tools/validate/board-check.sh`).
- **What the patches change.**  `rustc-patches/0004`: std's `libc` comes from
  the fork (`[patch.crates-io]` in `library/Cargo.toml`, the path relative to
  the sysroot's `rust-src` link, so the fork lives inside the rust checkout);
  `klausscpu` in std's per-arch lists (`stat` from libc, `MIN_ALIGN` 8, the
  EH data registers R0/R1); `unwind` links LLVM's libunwind (`-lunwind`) and
  has `_Unwind_Exception`'s two private words.  `backtrace/0001`: backtrace-rs
  picks the ELF class from the pointer width; KlaussCPU's 64-bit pointers in
  ELF32 files gave `<unknown>` for every frame.  `libc-patches/`: the
  `klausscpu` module (from riscv64's, with musl's syscall numbers, signal
  context and `ipc_perm`), and `dl_phdr_info` pointing at `Elf32_Phdr`.
- **A stale std.**  cargo does not notice a change to the `libc` fork (or
  the backtrace submodule) in `rust-src`: delete the project's `target/`
  after changing them.  The symptom can be strange: a std still built with
  the 64-bit `dl_phdr_info` panicked inside symbol lookup while holding
  std's backtrace lock, and the panic handler then waited on the same lock
  forever (std's `error` test hung).
- **`cacheflush`** (syscall 244, `linux/patches/0005` in the runtime) is in
  `libc` as `SYS_cacheflush`; musl's `bits/syscall.h.in` does not have it yet.

**Testing.**

- `linux-std/stdcheck`: `klauss-linux-cargo run --release` in it (and with
  `KLAUSSCPU_LINUX_DYNAMIC=1`) prints `stdcheck: PASS`, 26 checks.
- `linux-std/library-tests.sh`: Rust's own `coretests`, `alloctests` and
  std's integration tests (the Windows ones left out), copied from the
  checkout and built at opt-level 2 with debug assertions and overflow
  checks, std included; one binary at a time on the board, about 50 minutes.
- `linux-std/libc-test/`: the `libc` fork against musl's and the kernel's
  headers (upstream's ctest for every constant, type, field and function,
  plus function signatures and the C primitive types); see its README.

Results (2026-11-18, bitstream tmg2, Linux 7.3.3):

| Check | Result |
|---|---|
| `stdcheck`, static and dynamic (PIE) | 26/26 each, `stdcheck: PASS` |
| `coretests` | 2768 passed, 2 ignored, 0 failed (411 s) |
| `alloctests` | unit 333, integration 1331, `vec_deque_alloc_error` 1; 0 failed (29 min) |
| std's integration tests | 27 binaries, all pass: `sync` 337 (1 ignored), `path` 35, `num` 25, `time` 15, `error` 14, `thread_local` 11, `env` 8, `env_modify` 7 (1 ignored), `thread` 5, `process_spawning`, `pipe_subprocess`, `panic`, ... |
| `library-tests.sh` total | 30 of 30 binaries passed, about 50 minutes |
| `libc-test` | ctest 9944 checks + 98 in the `linux_*` groups, fnsig 740/740 functions, prims 22/22 types; 0 failed |
| the same with the built-in target (patch 0005: PIC, thread-locals) | `library-tests.sh` 30/30 (`thread_local` 12), `stdcheck` static and dynamic PASS |

## 8. rustc and cargo on the board

klausscpu-runtime `RUST_ON_BOARD_PLAN.md` builds rustc and cargo to run on
the board's Linux: `rustc hello.rs && ./hello` on the board, linking with
the board's clang (`/opt/llvm`).

- **Patch 0005** makes `klausscpu-unknown-linux-musl` a built-in target
  (bootstrap only builds a compiler for one), with `Arch::KlaussCpu`.  Its
  `rustc_codegen_ssa` part writes the object files rustc makes itself
  (metadata wrappers, a dylib's `.rustc` metadata, the linked-symbols object
  that keeps a dylib's exported code) as ELF32-LE i386 objects with
  KlaussCPU's `e_machine` and `e_flags`: the `object` crate cannot write
  KlaussCPU ELF.  Without it, `librustc_driver.so` linked at 4 MB.
- **Patch 0006 and `cargo/`** take `libc`, `linux-raw-sys` and `rustix` from
  forks for rustc and cargo, and `cc` for bootstrap (its compiler detection
  parses the triple).  `linux-raw-sys` maps klausscpu to riscv64's generated
  modules (both asm-generic LP64; signed `char`), and refuses `elf`,
  `elf_uapi`, `auxvec` and `ptrace`, whose layouts differ.
- **`rustc-patches/bootstrap-cross.toml`** (fill in the paths): host and
  target `klausscpu-unknown-linux-musl`, its own build directory
  (`build-cross`, so `build/` stays the cross compiler), the board's LLVM
  libraries through klausscpu-runtime `linux/rustc/llvm-config`, the target
  compilers `linux/rustc/klausscpu-clang{,++}`, dynamic host tools.
  `KLAUSSCPU_UNKNOWN_LINUX_MUSL_OPENSSL_DIR=<runtime>/linux-ws/br-output-dyn/staging/usr`
  for cargo (the board's OpenSSL).
  `./x.py build --config bootstrap-cross.toml --stage 2 compiler/rustc library cargo`
  (about 15 minutes from a built stage1).

The rest (the board's LLVM libraries, the image at `/opt/rust`, results) is
in the runtime plan.

## 9. Rebasing the pins

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
git -C <klausscpu-rust>/libc-klausscpu format-patch HEAD~3..HEAD \
    -o <this-repo>/llvm/lib/Target/KlaussCPU/rust/libc-patches/   # the commits on 0.2.185
```

The other forks likewise into `crate-patches/<crate>-<version>/`
(`git format-patch <first commit>..HEAD` in each).  When rust master or cargo
moves to a newer version of a forked crate, add that version to
`setup-crate-forks.sh` (the patches have so far applied unchanged) and point
the `[patch.crates-io]` entry at it.

When rust master moves to a newer `libc`, rebase `libc-patches/` onto that
version and change the crates.io version in part 2; then re-run
`linux-std/libc-test/`.
