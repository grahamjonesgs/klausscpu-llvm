# Rust on KlaussCPU — Enablement Plan

Plan for bringing the Rust toolchain up on the KlaussCPU backend in this LLVM
fork, producing programs that run (a) bare-metal via the serial loader and
(b) as LLEXT loadable extensions (`run foo.llext`) on the existing C-based
Zephyr 3.7 port in [`klausscpu-runtime`](https://github.com/grahamjonesgs/klausscpu-runtime).

---

## 0. What we build on (current state)

| Component | State |
|---|---|
| LLVM backend (`llvm/lib/Target/KlaussCPU/`) | Complete: 120+ instructions, ELF object emission, AsmParser, DWARF; hardware-confirmed |
| Clang | Full target (`klausscpu-unknown-elf`), driver toolchain, builtins |
| LLD | `EM_KLAUSSCPU` / `elf32klausscpu`; relocations ABS32, ABS64, PCREL32 |
| compiler-rt builtins | Built via `build-builtins.sh` (soft-FP, 64/128-bit divide, `__multi3`, …) |
| libc | picolibc (bare-metal); Zephyr minimal libc (kernel) |
| Zephyr 3.7 LTS | Boots on Nexys A7; SSH shell; LLEXT `run` command loads ELFCLASS32 `ET_REL` C extensions, working on hardware |
| Rust | Nothing yet |

Architecture facts that drive every decision below:

- 16×64-bit GPRs, 32-bit PC/SP, 128 MiB RAM, little-endian.
- DataLayout: `e-m:e-p:64:64-i64:64-i128:128-n32:64` (pointers/`usize` = 64-bit,
  `c_int` = 32-bit).
- Hardware MUL/MULH/DIV/MOD (signed+unsigned), shifts/rotates, CLZ/CTZ/POPCNT/
  BITREV/BSWAP, MIN/MAX, sign/zero-extend ops, sign-extending sub-word loads.
- **No** hardware FP (soft-float), **no** atomics, **no** CMOV (SELECT lowered
  to branches), **no** MMU/guard pages; single core.
- i128 supported: returned in R12+R11; compiler-rt 128-bit intrinsics work.
- Object format: ELFCLASS32 / ELFDATA2LSB / `EM_KLAUSSCPU = 0x4B43`; static
  relocation model only (no GOT/PLT/PIC); relocs `R_KLAUSSCPU_ABS32(1)`,
  `ABS64(2)`, `PCREL32(3)`.

---

## 1. Strategy overview

Rust's compiler uses LLVM for codegen, so the heavy lifting (the backend) is
already done. The work splits into five phases:

1. **Phase 1 — rustc target bring-up**: build `rustc` from source against this
   LLVM fork; add a `klausscpu-unknown-none-elf` target; `no_std` hello world
   on hardware (bare-metal `.bin`, same flow as `hello.c`).
2. **Phase 2 — core + alloc**: `-Zbuild-std=core,alloc`, global allocator,
   runtime support crates (`klauss-mmio`, `klauss-io`, …); fix any backend
   pattern gaps Rust exposes.
3. **Phase 3 — Rust `.llext` on Zephyr** (the flagship goal): produce
   `ET_REL` extensions from Rust that the existing SSH `run` command loads,
   linking against the kernel's `llext_exports.c` symbol table.
4. **Phase 4 — richer IO**: filesystem (Zephyr `fs_*` over FatFs/SD), MMIO
   peripheral crates, stdin/stdout routed per SSH session.
5. **Phase 5 (stretch)** — `std`-on-Zephyr PAL, `asm!` support, LLVM
   disassembler for `llvm-objdump`.

**Recommended target model**: `no_std + alloc` is the sweet spot. A full `std`
port is possible later (Phase 5) but is weeks of work for marginal benefit on
a 128 MiB single-core FPGA system; almost all embedded Rust ecosystem crates
are `no_std`-first.

---

## 2. rustc work items (Phase 1)

### 2.1 Building rustc against this fork

rustc normally bundles its own LLVM; instead we build it against this fork's
`build/` (which has the KlaussCPU target):

- `bootstrap.toml`:
  ```toml
  [llvm]
  download-ci-llvm = false
  [target.x86_64-unknown-linux-gnu]
  llvm-config = "/path/to/klausscpu-llvm/build/bin/llvm-config"
  ```
- **Version pinning**: this fork tracks LLVM trunk (v23). rustc accepts a
  narrow external-LLVM version window — pin a rustc nightly/commit whose
  supported LLVM range includes 23 and record it in CI. Re-pin whenever the
  fork rebases.
- The LLVM build must include the target:
  `-DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD="KlaussCPU"` (already the documented
  config) and be a **shared/static lib build usable by rustc** (Release +
  assertions recommended).
- Wire target initialization into rustc (small fork of rust-lang/rust,
  ~5 files — same pattern the Xtensa/esp fork uses):
  - `compiler/rustc_llvm/build.rs`: add `"klausscpu"` to `optional_components`.
  - `compiler/rustc_llvm/src/lib.rs`: `init` entries —
    `LLVMInitializeKlaussCPUTarget{Info,,MC,AsmPrinter,AsmParser}`.
  - `compiler/rustc_codegen_llvm/src/llvm_util.rs`: add the component gate.

> Maintain this as a small patch series on a `klausscpu` branch of
> rust-lang/rust (new repo `klausscpu-rust`), exactly like the LLVM fork.

### 2.2 Target specification

Add a **built-in** target (not just `--target=*.json` — see 2.3 for why):
`compiler/rustc_target/src/spec/targets/klausscpu_unknown_none_elf.rs`

```rust
Target {
    llvm_target: "klausscpu-unknown-elf",
    pointer_width: 64,
    arch: "klausscpu",
    data_layout: "e-m:e-p:64:64-i64:64-i128:128-n32:64", // MUST byte-match TargetMachine
    options: TargetOptions {
        endian: Endian::Little,
        c_int_width: 32,
        os: "none".into(),
        env/vendor: "",
        linker_flavor: Gnu(Cc::No, Lld::Yes),
        linker: Some("ld.lld"),            // the fork's lld, not rust-lld (see note)
        pre_link_args: ["-m", "elf32klausscpu"],
        cpu: "generic",
        features: "",
        max_atomic_width: Some(0),         // no atomics at all
        atomic_cas: false,
        panic_strategy: PanicStrategy::Abort,
        relocation_model: RelocModel::Static,
        executables: true,
        dynamic_linking: false,
        eh_frame_header: false,
        emit_debug_gdb_scripts: false,
        singlethread: true,                // affects LLVM atomic lowering
        ..Default::default()
    },
}
```

A second target `klausscpu-zephyr-elf` (Phase 3) differs only in `os:
"zephyr"` and defaults tuned for `ET_REL` extension output; sharing a
`base/klausscpu.rs`.

**Note on linkers**: with an external LLVM, bootstrap won't produce a
`rust-lld` containing KlaussCPU. Always link with the fork's `build/bin/ld.lld`
(set via target spec `linker` or `.cargo/config.toml`).

### 2.3 Why a rustc patch is unavoidable (JSON spec is not enough)

A pure JSON target spec fails for a *new architecture* because rustc matches
on `target.arch` in several places that panic/mis-compile on unknown arches:

- `compiler/rustc_target/src/callconv/mod.rs` (`adjust_for_foreign_abi`) —
  needs a `klausscpu` module. Keep it trivial: all scalars ≤ 64-bit direct in
  registers per the C CC; i128 as a pair (backend already splits/returns in
  R12+R11); aggregates > 16 bytes passed **indirectly** (pointer), matching
  what Clang does today so Rust↔C FFI agrees. Model on `msp430`/`riscv`'s
  simple implementations.
- `compiler/rustc_target/src/asm/` — only needed when we add `asm!` (Phase 5);
  until then `asm!`/`global_asm!` are compile errors on this target, which is
  acceptable (we use C shims, §5.3).
- `cfg(target_arch = "klausscpu")` becomes available for the support crates.

### 2.4 Building the sysroot

No prebuilt sysroot: use cargo's `-Zbuild-std`:

```sh
cargo +klausscpu build --release \
  --target klausscpu-unknown-none-elf \
  -Zbuild-std=core,alloc \
  -Zbuild-std-features=compiler-builtins-mem
```

- `compiler_builtins` (Rust's port of compiler-rt) compiles for new arches out
  of the box (pure Rust paths); it supplies `__addsf3`/`__udivti3`/`memcpy`/…
  inside every Rust artifact. The C `libclang_rt.builtins.a` stays for C-only
  links; mixed links are safe (weak/duplicate-tolerant), but prefer the Rust
  one inside `.llext` objects so extensions are self-contained (§6.3).
- Soft-float: LLVM expands all f32/f64 ops to libcalls (existing
  `ISelLowering` config); `compiler_builtins` provides them. `core::fmt` for
  floats (Ryū/Grisu) is pure integer code — works.

### Phase 1 exit criteria

`#![no_std] #![no_main]` hello world: panic-abort, a `_start` shim, UART
output through the C shim library (§5.3), linked with the existing
`klausscpu.ld`, converted with `klausscc -e`, prints on the board.

---

## 3. ISA review — needed updates and new opcodes

**Bottom line: no ISA change is *required* to run Rust.** The gaps below shape
the port and are listed with their software workaround and the hardware fix
that would remove the workaround.

### 3.1 Required: none

Current opcodes cover everything `core`/`alloc` codegen emits: full integer
ALU incl. MULH (used by 128-bit multiply), divide/modulo, variable shifts,
sub-word loads/stores with sign/zero extension, indirect call/branch (function
pointers, vtables, jump tables), and i128 via register pairs.

### 3.2 Interrupt masking — MMIO, contract **CONFIRMED** by the FPGA session

- **What exists**: a flat MMIO interrupt controller at `0xF00F_0000`
  (`INT_MASK` bitmask, `INT_PEND`, `INT_VEC(n)` vectors); interrupt entry
  pushes an 8-byte IRET frame (PC + flags/`INT_MASK`) and clears the
  dispatched source's mask bit; `IRET (0x6011)` atomically restores PC,
  flags, and the full mask. Zephyr's `arch_irq_lock()` is already a
  read-then-clear of `INT_MASK`, race-free given the entry/IRET save-restore
  (analysis in `FPGA_HANDOFF_IRQ.md` §5).
- **Why Rust cares**: with `max_atomic_width = 0`, `core::sync::atomic` types
  don't exist on the target. The ecosystem answer is the
  [`portable-atomic`](https://crates.io/crates/portable-atomic) crate with its
  `critical-section` feature: on single-core it emulates every atomic by
  masking interrupts around a plain load/store. MMIO is *ideal* for Rust here:
  `read_volatile`/`write_volatile` on `INT_MASK` compiles from pure stable
  Rust — no builtins, no `asm!`, no C shim.
- **Confirmed contract** (`FPGA_HANDOFF_IRQ_RESPONSE.md`, 2026-06-12 — all
  four questions confirmed; the core is a multi-cycle FSM, so IRQs dispatch
  only at the instruction boundary):
  - **Q1**: mask writes are zero-window — the mask updates *before* the store
    retires; **no read-back fence needed**; the §5 sequence is correct as
    written. The `IEXCHR` fallback opcode is dead — do not build it.
  - **Q2**: entry stacks the pre-entry mask; IRET restores PC + flags + mask
    on one edge. Rule: **an ISR changes the mask persistently by patching
    frame bits [42:39]**, never by direct MMIO write (IRET overwrites it).
  - **Q3**: entry clears only the dispatched source's bit (ISRs are nestable
    by other sources once sources 1–3 are wired — today only source 0/timer
    exists); pending is latched while masked and **coalesced** (N events →
    one delivery); reads are side-effect-free; a source with vector 0 never
    dispatches.
  - **Q4**: IRET high word: `hi[10:7]` = 4-bit `INT_MASK` (frame bits
    [42:39]), `hi[6:0]` = flags (zero/equal/carry/overflow/sign/less/ult),
    low 32 = resume PC. `0x80` = mask bit 0 set, flags clear.
  - Register-map corrections that matter to runtime code: `INT_MASK` bit 1
    is **not** ethernet yet (bits 1–3 reserved until Phase 6 wiring);
    `0xF00F_0030` = `TIMER_PERIOD`; `0xF00F_0038` is the timer **period
    counter** (wraps every rollover — *not* free-running); free-running time
    is `0xF00F_0040` `CLOCK_MS` (64-bit ms, read with `MEMGET64`).
- **Toolchain plumbing (unblocked)**: `klauss-critical-section` crate (pure
  Rust over `INT_MASK`) implementing the `critical-section` trait;
  `portable-atomic` on top; Zephyr's `arch_irq_lock()` already matches the
  same contract. Known follow-up in `klausscpu-runtime`: Zephyr's
  `arch_k_cycle_get_32()` reads `0xF00F_0038` as if free-running — it must
  move to `CLOCK_MS`-derived time (or period-counter + tick accumulation).

### 3.3 Optional (performance / future, not needed for this plan)

| Opcode idea | Benefit | Verdict |
|---|---|---|
| CAS or LL/SC (AMO) | True atomics; needed only for SMP or lock-free IRQ-vs-thread code | Defer; single core + IRQ masking is sufficient and what most MCU-class Rust targets do |
| CMOV / select | Removes branch pairs from `min`/`max`/`clamp`-style code | Perf only; SELECT_PSEUDO works today |
| `LDIDX`/`STIDX` reg+reg addressing | Tighter array loops | Perf only |

### 3.4 Relocations / object format: no additions needed

Rust codegen at `relocation-model=static` emits exactly what C does:
- code refs → `R_KLAUSSCPU_ABS32` (SETR/CALL slots), `PCREL32` (REL branches);
- data pointers (vtables, `&'static str` fat-pointer halves, slice tables,
  `core::fmt` argument tables) → `R_KLAUSSCPU_ABS64` in `.rodata`/`.data`;
- switch jump tables (`EK_Custom32`) → `ABS32`.

No GOT/PLT/TLS relocs are needed (no dynamic linking, no `thread_local!` in
`no_std`). The existing lld and `arch_elf_relocate` cover all of it.

### 3.5 LLVM backend work items (this repo) exposed by Rust

Not ISA changes — selection/robustness items to do during Phase 2:

1. **Audit isel patterns for the bit-manipulation ops Rust leans on**:
   `ISD::CTLZ/CTTZ/CTPOP/BSWAP/BITREVERSE/ROTL/ROTR/SMIN/SMAX/UMIN/UMAX/ABS`
   should select to `CLZ/CTZ/POPCNT/BSWAP_R/BITREV/ROLR/RORR_R/MINR/MAXR/…`
   rather than expand. `u32::leading_zeros`, `NonZero*`, hashbrown, and float
   formatting hit these constantly. Add `.ll` regression tests per op.
2. **`MULHS`/`MULHU` patterns → `MULHR`/`MULHUR`** (128-bit multiply in
   `compiler_builtins`); verify `umulo`/`smulo` (Rust overflow checks in debug
   builds!) expand correctly — `checked_*`/`overflowing_*` arithmetic is
   everywhere in debug-assertions builds.
3. **i128 call args**: returns are done (R12+R11); add tests that i128
   *arguments* (legalizer-split into two i64s across R0–R3/stack) round-trip
   against C `__int128`, since `compiler_builtins` intrinsics take i128 args.
4. **Stack growth**: Rust has no stack probes here and the CPU has no guard
   pages — document per-thread stack sizing; consider an optional
   `-Z stack-size` lint in docs rather than backend work.
5. **Disassembler** (`llvm/lib/Target/KlaussCPU/Disassembler/`) — optional but
   makes `llvm-objdump -d` work on Rust artifacts; big QoL for debugging
   `.llext` relocation issues.

---

## 4. Standard libraries: what gets ported, what doesn't

| Layer | Action |
|---|---|
| `core` | **Compiles unmodified** once the target exists. Soft-float via libcalls, i128 OK. `core::sync::atomic` absent until §3.2 + `portable-atomic`. |
| `alloc` | **Compiles unmodified**; needs a `#[global_allocator]` (below). `Arc` needs atomics → available via `portable-atomic-util` once §3.2 lands; `Rc`, `Vec`, `String`, `BTreeMap`, `Box` work day one. |
| `std` | **Not ported initially** (Phase 5 stretch, see below). |
| `compiler_builtins` | Comes free with `-Zbuild-std`; replaces `libclang_rt` inside Rust artifacts. |
| `panic` runtime | `panic=abort`. Bare metal: `#[panic_handler]` prints via UART shim then `core::intrinsics::abort()` → `HALT_I`. LLEXT: handler must **not** halt the kernel — print to the session and terminate only the extension (§6.5). |

### Global allocator

- **Bare metal**: pure-Rust `linked_list_allocator` over the region between
  `__bss_end` and `_stack_top` (or wrap the existing `libc.c` heap for
  C-interop parity).
- **LLEXT**: thin `GlobalAlloc` over the kernel-exported
  `malloc/calloc/realloc/free` (already in `llext_exports.c`) — one shared
  heap with the kernel, same as C extensions today.

### `std` (Phase 5 stretch) — scope if ever wanted

A real `std` port means a new PAL: `library/std/src/sys/pal/klausscpu_zephyr/`
mapping onto kernel exports:

| std module | Backing Zephyr/kernel API |
|---|---|
| `std::io` stdin/stdout | exported `putchar`/`getchar` (per-thread SSH routing already exists) |
| `std::fs` | `fs_open/fs_read/fs_write/fs_seek/fs_close/fs_readdir/…` (FatFs on SD) |
| `std::time` | `k_uptime_get` + the SNTP wallclock in `ssh/wallclock.c` |
| `std::thread` | `k_thread_create/abort/sleep` (needs atomics first) |
| `std::net` | Zephyr BSD sockets (kernel already runs networking for SSH) |
| `std::process`, `env` | stub `ENOSYS` |

Estimate: 4–8 weeks incremental. **Recommendation: don't** — `no_std + alloc`
plus the crates in §5 delivers ~all practical value at ~15% of the cost.

---

## 5. IO shims and support crates

New `rust/` workspace in the **`klausscpu-runtime`** repo (this LLVM fork
stays runtime-free, matching the repo split):

```
rust/
├── klauss-sys            raw extern "C" decls for kernel exports + bare-metal libc
├── klauss-alloc          #[global_allocator] over malloc/free
├── klauss-io             print!/println!/dbg! + Stdin (core::fmt::Write impls)
├── klauss-mmio           typed volatile peripheral access (port of mmio.h)
├── klauss-fs             safe wrappers over Zephyr fs_* (Phase 4)
├── klauss-llext-rt       extension entry/panic/alloc glue (one `use` per program)
├── klauss-rt             bare-metal _start/panic glue (counterpart of crt0.c)
└── examples/             hello, alloc-demo, fs-demo, adventure-rs, …
```

### 5.1 Stdio

- `klauss-io` implements `core::fmt::Write` over a byte sink:
  - **LLEXT build** (feature `llext`): sink = exported `putchar`; input =
    exported `getchar`. The kernel's per-thread console hooks
    (`arch_printk_char_out` / `klausscpu_console_in_hook`) already route these
    to the right SSH session — Rust needs *zero* extra work for concurrent
    sessions.
  - **Bare-metal build** (feature `bare`): sink = `uart_putc` from the C shim
    (§5.3).
- `print!`/`println!` macros mirror `std`'s; formatted output goes through
  `core::fmt`, so no dependence on C `printf` semantics (avoids the picolibc
  `%llu` and cbprintf packaging pitfalls entirely).

### 5.2 MMIO

Direct Rust port of `mmio.h` (MMIO bus at `0xF000_0000`, device id in
`addr[27:16]`): SD `0xF000_xxxx`, UART regs `0xF001`, RGB `0xF002`, 7-seg
`0xF003`, LEDs/switches `0xF004`, cache counters `0xF005`, system timer
`0xF00F_0000`, Ethernet, crypto block (`crypto_hw.h`).

- Implementation: `core::ptr::read_volatile`/`write_volatile` behind typed
  register structs (svd2rust-style API by hand; the device count is small).
  Plain volatile loads/stores compile to `LDIDX*/STIDX*` — no intrinsics
  needed, works today.
- Mark everything `unsafe` at the raw layer; safe HAL wrappers (`leds()`,
  `seg7()`, `Rgb::set()`, SD block driver) above it. Optionally implement
  `embedded-hal` traits to unlock the ecosystem.

### 5.3 The UART/builtin shim (bare metal only)

`TXR/TXCHARMEMR/TXSTRMEMR/RXRB/DELAYV/LEDR/SEG7R` are *opcodes*, reachable
from C via `__builtin_klausscpu_*`. Rust cannot call clang builtins and the
target has no `asm!` yet, so:

- Ship `libklauss_ioshim.a`: a tiny C file (essentially today's
  `uart_stubs.c` + `io_stubs.c`) compiled by the fork's clang, exposing
  `extern "C"` functions (`uart_putc`, `uart_getc`, `hw_delay`, `leds`, …).
  `klauss-sys` declares them; build script links the archive.
- LLEXT builds don't need it (kernel exports cover console IO).
- Phase 5 removes the shim by adding KlaussCPU `asm!` support to rustc
  (`rustc_target/src/asm/`), or by teaching rustc the LLVM intrinsics
  (`llvm.klausscpu.*` already exist — a `link_llvm_intrinsics` path works on
  nightly as an interim).

### 5.4 Filesystem (Phase 4)

Extend `ssh/llext_exports.c` with the Zephyr fs API surface
(`fs_open`, `fs_close`, `fs_read`, `fs_write`, `fs_seek`, `fs_tell`,
`fs_stat`, `fs_unlink`, `fs_mkdir`, `fs_opendir`, `fs_readdir`,
`fs_closedir`) and wrap in `klauss-fs` (`File`, `Dir`, `read_to_string`,
iterator over dir entries; paths are `/SD:/...` strings). This benefits C
extensions too.

---

## 6. Producing `.llext` files loadable by the Zephyr ssh `run` command

This is the critical pipeline. The loader consumes **ELFCLASS32 `ET_REL`**
objects, resolves UND symbols against `llext_exports.c`, copies sections into
the LLEXT heap, and applies `R_KLAUSSCPU_{ABS32,ABS64,PCREL32}` via
`arch_elf_relocate` — all already proven with C extensions.

### 6.1 Build pipeline

```sh
# 1. Compile the program + core/alloc as a staticlib with fat LTO
cargo build --release --target klausscpu-zephyr-elf \
  -Zbuild-std=core,alloc -Zbuild-std-features=compiler-builtins-mem
#    profile: opt-level="s", lto="fat", codegen-units=1, panic="abort"

# 2. Partial-link every member into ONE relocatable object
ld.lld -r -m elf32klausscpu -o prog.llext.o \
    $(ar t prog.a | objects…)        # or: ld.lld -r --whole-archive prog.a

# 3. Strip debug info, KEEP the symtab (loader resolves `main` by name)
llvm-strip --strip-debug prog.llext.o
mv prog.llext.o prog.llext           # → copy to SD, `run prog.llext`
```

Wrapped in a `cargo-klauss` xtask / Makefile target (`make rust-ext-demos`)
mirroring the existing `make ext-demos`.

### 6.2 Why this shape works

- rustc emits objects through **this fork's** backend → correct machine
  (`EM_KLAUSSCPU`), class (ELFCLASS32), endianness, and only the three
  relocation types the loader implements (static model, no GOT — §3.4).
- `ld.lld -r` merges `core`/`alloc`/`compiler_builtins`/program into a single
  `ET_REL` with `SHT_RELA` sections — exactly what
  `CONFIG_LLEXT_TYPE_ELF_OBJECT` expects. The vendored
  `llext-klausscpu.patch` already handles LLVM's merged string tables and the
  `keep_symtab` flag.
- Entry point: `klauss-llext-rt` provides
  `#[no_mangle] extern "C" fn main(argc: c_int, argv: …) -> c_int` that calls
  the user's Rust `fn kmain()`. The loader resolves `main` by name, runs it on
  the SSH connection thread — identical lifecycle to C extensions, including
  concurrent sessions.

### 6.3 Symbol policy

- **Imports** (UND, resolved by loader): `putchar`, `getchar`, `printf?`,
  `malloc/calloc/realloc/free`, later `fs_*`, `k_msleep`, `k_uptime_get`.
  Keep the Rust import set documented in `klauss-sys` and assert at build time
  (xtask runs `llvm-nm -u` and diffs against the export list) so a missing
  export is caught on the host, not as `-ENOENT` at load.
- **Self-contained**: `compiler-builtins-mem` makes the extension carry its
  own `memcpy/memset/memcmp` and all FP/i128 intrinsics — no need to export
  compiler-rt symbols from the kernel and no version-skew risk.

### 6.4 Code-size control (the main practical risk)

`--gc-sections` cannot be combined with `ld.lld -r`, so dead-code removal must
happen **before** the partial link:

- fat LTO + `codegen-units=1` + `opt-level="s"` internalizes and drops unused
  `core`/`alloc` code at IR level — this is the primary mechanism;
- `panic=abort` + `panic_immediate_abort` (build-std feature) strips the
  formatting machinery from panics if size demands it;
- raise `CONFIG_LLEXT_HEAP_SIZE` (expect first Rust extensions in the
  100–300 KB range vs ~10 KB for C; tune later). 128 MiB RAM gives headroom.

Fallback if `-r` of LTO output hits a snag: single-crate
`--emit=obj` with `#![no_builtins]` workarounds — but the LTO+`-r` path is the
plan of record.

### 6.5 Panic behavior inside an extension

A panicking extension must not `HALT` the CPU or kill the kernel. The
`klauss-llext-rt` panic handler: print `panic: <msg>` via `putchar`, then
terminate only the extension. Mechanism: `main` wrapper runs the user code
under a `setjmp`-style anchor — concretely, export a tiny kernel helper
(`llext_exit(code)`, added to `llext_exports.c`, implemented with the loader's
existing thread context) the handler calls; the loader unloads the extension
as it already does on normal return. (Design note: pure-Rust unwinding is not
an option with `panic=abort`; this kernel-assisted exit is the simple, robust
choice.)

### 6.6 Zephyr-side work items

1. **Wire `arch/klausscpu/core/elf.c` into the module build** — the
   `arch_elf_relocate` implementation exists but is marked
   "PROPOSAL / NOT YET WIRED"; add it to `core/CMakeLists.txt` and move the
   `R_KLAUSSCPU_*` constants to `include/zephyr/arch/klausscpu/elf.h`.
   (If the current C `.llext` flow relocates via another path, consolidate on
   this hook so Rust and C share one relocator matching lld exactly.)
2. Extend `llext_exports.c`: `fs_*` set (§5.4), `k_msleep`, `k_uptime_get`,
   `llext_exit` (§6.5).
3. Bump `CONFIG_LLEXT_HEAP_SIZE` / malloc arena in `apps/ssh_shell/prj.conf`.
4. No changes needed to the per-session stdio routing — it is symbol-level
   and language-agnostic.

### 6.7 Test ladder (each step is a board-verified milestone)

1. `hello.llext` — `println!` over SSH.
2. `alloc.llext` — `Vec`/`String`/`BTreeMap` stress against the shared heap.
3. `panic.llext` — verify a panic terminates only the session's extension.
4. `fs.llext` — read/write/readdir on `/SD:/`.
5. `adventure-rs.llext` — port of `adventure.c`; interactive stdin/stdout,
   long-running concurrently with another session's `run hello.llext`.
6. Mixed regression: C extensions still load (same loader path).

---

## 7. Milestones, sequencing, risks

| Milestone | Contents | Depends on |
|---|---|---|
| **M1** ✅ | **Hardware-confirmed 2026-06-12.** stage1 rustc 1.98-dev vs this LLVM; JSON target spec + `Arch::Other` (no built-in target needed); `rust/hello-smoke` ran on the board (7-seg `0x518C9058` = const-folded `mix()`, LEDs mirror switches⊕0x55). rustc fork: `~/Documents/src/klausscpu-rust`, branch `klausscpu` | — |
| **M2** | build-std core+alloc; allocator; `klauss-{sys,io,mmio,rt}`; backend pattern audit (§3.5) + `.ll` tests | M1 |
| **M3** | Rust `.llext` via `run` on Zephyr; `klauss-llext-rt`; export-table checks; test ladder 1–3 | M2 + §6.6 |
| **M4** | `klauss-fs`; adventure-rs; docs (`RUST.md` in runtime repo); CI job building all examples | M3 |
| **M5** (opt) | `klauss-critical-section` over MMIO `INT_MASK` → `portable-atomic` (`Arc` etc.); `asm!`; disassembler; std PAL | none — §3.2 contract confirmed 2026-06-12; can start any time after M2 |

**Risks & mitigations**

- *rustc↔LLVM version skew* (highest): fork tracks LLVM trunk; rustc supports
  a bounded LLVM range. Pin (rustc commit ↔ fork commit) pairs; document in
  both repos; CI builds the pair.
- *`ld.lld -r` + LTO output quirks* (merged strtabs, section merging): the
  Zephyr-side patch already handles the known LLVM-isms for C; budget
  debugging time in M3; the disassembler (§3.5.5) pays for itself here.
- *Code size of `core::fmt`*: mitigations listed in §6.4; worst case Rust
  extensions use a leaner formatting crate (`ufmt`).
- *No stack guards*: document stack budgets; prefer heap recursion-depth
  limits in examples (same constraint C has today).
- *Debug-build overflow checks* hit `umulo/smulo` paths — covered by §3.5.2
  audit before anyone compiles at `opt-level=0`.

---

## 8. Summary of required changes by repository

| Repo | Changes |
|---|---|
| **this fork (klausscpu-llvm)** | §3.5 isel pattern audit + tests; optional Disassembler; (later) IRQ-mask opcodes + builtins; no relocation/ABI changes |
| **new `klausscpu-rust`** (rust-lang/rust fork) | LLVM component wiring; built-in target spec(s); `callconv/klausscpu.rs`; (later) `asm/klausscpu.rs` |
| **klausscpu-runtime** | `rust/` workspace (crates §5, examples, xtask packaging §6.1); `libklauss_ioshim.a`; Zephyr: wire `elf.c`, extend `llext_exports.c`, heap config |
| **hardware (Verilog)** | nothing — MMIO `INT_MASK` contract confirmed (`FPGA_HANDOFF_IRQ_RESPONSE.md`); `IEXCHR` fallback dead; board run of `test_irq_mask.c` T1–T4 as regression anchor when IRQ sources 1–3 get wired |
