# clang and lld that run on the board

The board's Linux has its own clang and lld, built from this repository. They
are at `/opt/llvm`, which is a read-only image on the SD card. A user can
`clang hello.c -o hello` on the board. The objects come out byte-identical to
the cross compiler's on the build host.

The build scripts live in **klausscpu-runtime**, not here. They need that
repository's sysroot: musl and the C++ runtime. This file says what the board
build is, how to remake it, and why its settings are what they are, so that
nobody "simplifies" them away. The full history and every measurement are in
klausscpu-runtime's `CLANG_ON_BOARD_PLAN.md`, milestones M1–M6. The day-to-day
commands are in its `LINUX_STATUS.md`.

## What the board binary is

- **One binary:** a single static multicall binary, `bin/llvm`
  (`LLVM_TOOL_LLVM_DRIVER_BUILD`). `clang`, `clang++`, `ld.lld`, `llvm-ar`,
  `llvm-nm`, `llvm-objdump` and `llvm-readobj` are symlinks to it.
- **Target:** `klausscpu-unknown-linux-musl` is both the host and the default
  target. Only the KlaussCPU target is built.
- **Build configuration:** Release (-O2), no assertions, no threads, no
  plugins, and no zlib, zstd, libxml2 or terminfo. It is linked against the
  runtime's musl + libc++ sysroot.
- **Profile-guided (PGO):** the build uses `LLVM_PROFDATA_FILE` with the
  committed profile `linux/toolchain/clang.profdata.xz` (in klausscpu-runtime).
- **ThinLTO:** `LLVM_ENABLE_LTO=Thin`, with `-Wl,--lto-O2` forced at the end of
  the link line. This is needed because AddLLVM links `llvm-driver`, which isn't
  a distribution component, with `--lto-O0`.
- **Function order:** the binary is linked with `--symbol-ordering-file` from
  `linux/toolchain/clang-order.txt.gz`. That file holds a call-graph order
  (lld's cdsort, "CDSX") made from klausscc's profile of this exact
  configuration. The order file is only valid for the configuration it was made
  from, because PGO and LTO change the symbol names and the call graph.

## Building it

From klausscpu-runtime, with `klausscpu-llvm/build/bin` as the host toolchain:

```sh
NATIVE_MULTICALL=1 NATIVE_BUILD_TYPE=Release NATIVE_PROFDATA=default \
NATIVE_LTO=Thin NATIVE_ORDER=1 \
NATIVE_BUILD=../klausscpu-llvm/build-native-klausscpu-pgo-lto \
  linux/toolchain/build-native-llvm.sh          # ~8 min on 24 cores
linux/toolchain/build-toolchain-image.sh        # linux-ws/toolchain.img
# on the board: umount /opt/llvm, then
gzip -1c linux-ws/toolchain.img | ssh root@<board> install-toolchain -z -
```

The header of `build-native-llvm.sh` documents every option.

**Check every new binary.** Run the runtime's
`linux/userland/tests/native-clang/board.sh`, which tests object and link
identity with the cross clang, then links and runs programs.
`NC_SKIP=cxx-stdlib` skips a one-hour compile.

## Remaking the profile and the order

The host build here must contain klausscpu-llvm bd5e2a47 or later. PGO
objects carry `.cg_profile`, which needs `.reloc BFD_RELOC_NONE`.

1. **Profile:** `linux/toolchain/pgo-profile.sh` in klausscpu-runtime, about
   15 minutes.
   - It builds an instrumented x86 clang of the same configuration
     (`build-pgo-instr`; delete it afterwards).
   - It runs that clang compiling *for KlaussCPU* on ten training compiles.
   - It merges the result into `clang.profdata.xz`.
   - **Held-out files:** bzip2's `decompress.c` and libc++'s `terminate.cpp`
     are never trained on, so they show whether the gain is general.
   - **Hash mismatches:** about 10% of functions report a CFG hash mismatch,
     because x86 and KlaussCPU builds differ slightly (libstdc++ vs libc++
     templates, 64- vs 32-bit). They lose their profile and nothing else.
   - **When to remake it:** the profile embeds source paths and function hashes,
     so it goes stale as LLVM changes. The build's "profile data may be out of
     date" warnings show how stale. Remake it after a big merge.
2. **Order:** build without `NATIVE_ORDER`, then follow klausscc
   `tools/tlbord/run.sh`'s header (the PGO + ThinLTO CDSX flow):
   - profile the unstripped binary on the emulator (`PROFILE=1`, the ten
     training workloads);
   - run symbolize, gen-orders, a cdsort relink and `cdsx.py`;
   - `gzip -9 -n` the result to `clang-order.txt.gz`;
   - rebuild with `NATIVE_ORDER=1`.
   - **Check identity:** `HASH=1` compares the objects the new binary makes in
     the emulator with the host clang's.

## Why these settings (measured)

**deflate.c -O2 on the board, user + sys:**

| step | time |
|---|---|
| MinSizeRel, before any hardware help | 109 s |
| Release (-O2): 24% fewer instructions | 90 s |
| hardware TLB walker, I-cache, L2, call-graph order | 37.6 s |
| PGO + ThinLTO + a fresh call-graph order | 30.2 s (−20%) |

The C++ hello -O2 (preprocessed) went from 88.6 s to 77.5 s (−12.5%) in that
last step. On the emulator the held-out files gained 12–16%.

**PGO alone vs with ThinLTO:**
- PGO alone gave about −11%, and its text shrank from 90.7 to 82.2 MB.
- ThinLTO about doubled the gain. Its text is 94 MB and peak RSS is about 9 MB
  higher, but the board showed no page-cache pressure (0–5 major faults).
- **Function order:** lld's own cdsort from the PGO call-graph profile was
  worse than the klausscc-profiled CDSX order. Without CDSX, main-TLB refills
  rose 30–80%.
- **Speed limit:** clang's speed on the board is bounded by instruction fetch.
  Hot code is several MB, against a 64 KiB I-cache and a 128 KiB L2. That's why
  code layout (the order, PGO) mattered more than anything else in software.
