# KlaussCPU LLVM Backend

LLVM backend for the KlaussCPU — a home-designed 64-bit CPU implemented in
SystemVerilog on a Digilent Nexys A7 (Artix-7) FPGA.  It is an experimental
target in this LLVM fork, together with its clang target and driver, an lld
port (`ld.lld -m elf32klausscpu`) and an lldb ABI plugin.

- **ISA**: defined in the hardware repo `klaussCPU` —
  `CPU_ARCHITECTURE.md`, `ISA_ENCODING_V2_MAP.md` (the encoding map),
  `ISA_V3_IMPL.md`.
- **Runtime, programs and RTOS ports** (bare metal, FreeRTOS, lwIP, Zephyr,
  LLEXT, picolibc, compiler-rt builtins): the separate
  [`klausscpu-runtime`](https://github.com/grahamjonesgs/klausscpu-runtime)
  repo, which builds with this fork's `build/bin` (`KLAUSSCPU_LLVM_BIN`).
- **Backend internals** (ABI, frame layout, flags, encoding, how to add an
  instruction, options): [`CLAUDE.md`](CLAUDE.md).

---

## Build

```bash
cmake -G Ninja -S llvm -B build \
  -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_TARGETS_TO_BUILD="X86" \
  -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD="KlaussCPU" \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_OPTIMIZED_TABLEGEN=ON

ninja -C build llc llvm-mc clang lld \
  llvm-objcopy llvm-objdump llvm-readelf llvm-readobj llvm-dwarfdump \
  llvm-ar llvm-nm llvm-ranlib llvm-strip llvm-size
```

`X86` (or whatever the host is) is there for LLVM's host tools; it does not
affect KlaussCPU output.  `llvm-readelf` is needed by Zephyr's post-build
step.  The compiler-rt builtins (soft float, 64/128-bit division) are built
into the clang resource directory by `build-builtins.sh` in
`klausscpu-runtime`; re-run it after a `build/` wipe.

## Use

```bash
clang --target=klausscpu-unknown-elf -O2 -c foo.c          # object
clang --target=klausscpu-unknown-elf -O2 -S foo.c          # assembly
llvm-mc -triple=klausscpu-unknown-elf -filetype=obj foo.s  # assembler
llvm-objdump -d foo.o                                      # disassembler
llc -march=klausscpu foo.ll
```

The clang driver adds `-ffreestanding` and links with `ld.lld`.  `int` is
32-bit; `long`, `long long` and pointers are 64-bit.  The assembler takes
mnemonics in either case, registers `r0`–`r15` or `a`–`p`, and GAS
directives.

## Test

```bash
build/bin/llvm-lit -q llvm/test/CodeGen/KlaussCPU llvm/test/MC/KlaussCPU \
  llvm/test/MC/Disassembler/KlaussCPU llvm/test/DebugInfo/KlaussCPU \
  llvm/test/Transforms/LoopUnroll/KlaussCPU lld/test/ELF/klausscpu-*.s
```

## Other documents in this directory

| File | What it is |
|---|---|
| `CLAUDE.md` | current backend reference |
| `HISTORY.md` | development log (superseded statements kept as written) |
| `ISA_ENCODING_V2.md` | historical snapshot of the v2 encoding map (see its header) |
| `ISA_V3_PROPOSAL.md`, `M8_SCHED_MODEL_SPEC.md`, `FLAG_UNIFICATION_CHANGES` | design notes for ISA v3, the scheduling model and the flag unification |
| `FPGA_*.md`, `*_HANDOFF.md`, `RTOS_NOTES.md` | hardware errata and handoff notes |
| `RUST_PLAN.md`, `rust/` | Rust target spec, rustc patches and demo crates |
