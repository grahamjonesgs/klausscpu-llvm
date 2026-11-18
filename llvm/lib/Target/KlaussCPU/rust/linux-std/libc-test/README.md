# libc-test for klausscpu-unknown-linux-musl

Checks the `libc` crate fork (`libc-klausscpu`, the copy std is built with;
its commits are `../../libc-patches/`, on top of pristine libc 0.2.185)
against KlaussCPU's real C headers: musl 1.2.5 and the Linux 7.3 uapi headers
in the Linux sysroot.

## Run it

```
./run.sh              # build, host checks, then the ctest binaries on the board
NO_BOARD=1 ./run.sh   # build and host checks only (no board needed)
./summary.sh          # re-print the board results in the work directory
```

`run.sh` exits 0 only if everything passes. A full run takes about 5 minutes
of building (std is built from source each time the fork changes) and a few
minutes on the board, plus any time waiting for the board locks.

Only sources are in this directory. `setup.sh` (run by `build.sh`) fetches
upstream rust-lang/libc tag `0.2.185` (shallow `git clone`, needs network once;
the crates.io tarball has no libc-test) into the work directory, copies
upstream's `libc-test/tests/*.rs` and `src/*.c` (used verbatim), applies
`crate/build.rs.patch` to upstream's `libc-test/build.rs`, and generates
`Cargo.toml`, `.cargo/config.toml` and the `core-only` crate there. Everything
generated (clone, target directories, expanded crate, results) is in the work
directory, `~/.cache/klausscpu-libc-test` by default.

Requirements: the patched stage1 rustc, `cargo +nightly`, the fork's
`build/bin` (clang, llvm-ar), the Linux sysroot, python3, `git`, `patch`,
network access for the first run, and for the board part the SSH key of
`../../klauss-linux-run`. Environment (the same names as `../../klauss-linux-cargo`
and `../../klauss-linux-run`; see `env.sh`):

- `KLAUSSCPU_RUSTC` the stage1 rustc (required, or from `../../.klaussbuild.env`);
- `KLAUSSCPU_LLVM_BIN`, `KLAUSSCPU_LINUX_SYSROOT`, `KLAUSSCPU_CARGO_TOOLCHAIN`;
- `KLAUSSCPU_LIBC_SRC` the libc fork under test (default: `libc-klausscpu` in
  the rust checkout `KLAUSSCPU_RUSTC` was built in); the ctest crate depends
  on this path;
- `KLAUSSCPU_LIBC_TEST_WORK` the work directory;
- `KLAUSSCPU_BOARD`, `KLAUSSCPU_BOARD_KEY`; `KLAUSSCPU_BOARD_DIR` (default
  `/data/libc-test`).

The board runs go through `../../klauss-linux-run`, which holds both board
locks for each binary and writes only to `KLAUSSCPU_BOARD_DIR` (under `/data`).

## What is checked

1. **ctest** (`crate/`, the board part). Upstream `libc-test` from
   rust-lang/libc tag `0.2.185` (`libc-test` of the clone in the work directory), with
   `ctest =0.5.0-beta.3` as upstream's lockfile has it. `ctest` parses the
   libc crate as macro-expanded for the target, generates C (compiled by the
   klausscpu clang with `-Werror`) and Rust, and links them into one test
   binary per group. For every item that is reachable for
   `target_os = linux, target_env = musl, target_arch = klausscpu` it checks:
   - type aliases: size, alignment, signedness;
   - structs and unions: size, alignment, and per field the offset, the size
     and the type (the C compiler rejects a field whose Rust type translates
     to an incompatible C pointer type);
   - a by-value round trip of every struct through C and back (calling
     convention for aggregates, Rust codegen against clang);
   - constants: the value (and that C declares them at all);
   - functions and statics: that the Rust declaration resolves to the same
     address as the C one.

   `ctest` reports 9,944 tests from 8,787 generated test functions: 5,052
   constants, 328 types (size/alignment) with 77 signedness checks, 1,153
   struct fields (offset and size) and 1,125 field types, 324 by-value round
   trips, 726 functions and 2 statics, plus the small `linux_*` groups (fcntl seals,
   IPV6 flow-info, termios2/BOTHER, Elf32/64_Phdr, ARPHRD_CAN, XSI
   `strerror_r`) that upstream tests in separate binaries because of header
   clashes.

   `crate/build.rs.patch` turns upstream's `build.rs` into one with these
   KlaussCPU changes (search for `klausscpu` in the patched file):
   - `klausscpu` is treated as an LP64 musl 1.2.5 target
     (`RUST_LIBC_UNSTABLE_MUSL_V1_2_3` is set in the generated `.cargo/config.toml`);
   - **kernel constants are checked** (upstream skips `SYS_*`, `AF_*`, `F_*`,
     `MS_*`, `TCP_*`, ... on every non-glibc target). Set
     `KLAUSSCPU_LIBC_TEST_SKIP_KERNEL_CONSTS=1` to get upstream's behaviour;
   - items upstream skips only because its CI's musl is older than 1.2.5 are
     checked (`statx`, `preadv2`/`pwritev2`, `gettid`, `reallocarray`,
     `pthread_getname_np`, `tcp_info`, `utmpx`, `RLIM_NLIMITS`,
     `SO_BUSY_POLL_BUDGET`, `SO_PREFER_BUSY_POLL`, `SYS_mseal`,
     `SYS_memfd_secret`);
   - `sys/reg.h` is included (`REG_SP`, `REG_PC`);
   - skipped, with the reason in `build.rs`: kernel constants musl's own
     headers do not declare (`STATX_*`, `TCP_COOKIE_TRANSACTIONS`,
     `MSG_COPY`); `KERN_REALROOTDEV` and `VM_LAPTOP_MODE`, which Linux 7.3's
     uapi no longer has; `SW_MAX`, `SW_CNT` and `FUTEX_CMD_MASK`, whose values
     changed in newer kernels (libc has the older ones; arch-independent);
     `SYS_cacheflush`, which is in the kernel's syscall table (244,
     `linux/patches/0005`) but not yet in musl's `bits/syscall.h.in` or the
     installed sysroot (the Rust value, 244, was checked by hand against
     `scripts/syscall.tbl`); `statx`'s field
     *types* (musl uses `uint64_t`, libc `__u64`; sizes and offsets are still
     checked) and its three newer fields musl keeps in `__spare`.

   Everything else upstream skips is still skipped, for upstream's reasons.

2. **fnsig** (`fnsig/fnsig.py`, host only). ctest compares only function
   addresses, not types. fnsig compares every `extern "C"` function of the
   expanded crate (including the ones ctest skips by name, e.g. `execv*`,
   `strtol*`, `*scanf`) with its C declaration at the ABI level: number of
   parameters, variadic or not, and for the return value and each parameter
   the class (integer / floating / pointer / aggregate / void), the size, and
   for integers the signedness. The C side is a C++ file (template deduction
   on `&function`, clang type-trait builtins, musl headers, no libc++)
   compiled for the target to assembly; the Rust side is parsed from the
   expanded crate with type aliases resolved. `NOTE` lines are 8-byte integer
   against pointer, which is the same in registers: `pthread_t` is an
   `unsigned long` in musl's C++ mode (a pointer in C, as in Rust) and libc's
   `sighandler_t` is a `size_t`.

3. **prims** (`fnsig/prims.py`, host only): `core::ffi::c_*`, `usize`/`isize`
   and the fixed-width integers against clang's C types for the target (size,
   alignment, signedness of `char` etc.), as const assertions compiled for the
   target.

## Layout

- `setup.sh`, `build.sh`, `run.sh`, `run-board.sh`, `summary.sh`, `env.sh`.
- `crate/` what the ctest crate is made of: `build.rs.patch` (upstream's
  `build.rs` -> the KlaussCPU one; its tests are built as binaries so no
  libtest is needed), `Cargo.toml.in`, `Cargo.lock` (pins ctest
  `=0.5.0-beta.3` as upstream's lockfile has it), and
  `rustc-expand-wrapper.sh`, which lets ctest's `rustc -Zunpretty=expanded
  --target klausscpu-unknown-linux-musl` work with the JSON target: it adds
  `-Zunstable-options`, substitutes the JSON path, and supplies `core` from
  `core-only/`.
- `core-only/` an empty `no_std` crate built with `-Zbuild-std=core`, only to
  have a `core` rmeta for the target.
- `fnsig/` the host-only checks; the last fnsig report is
  `$WORK/fnsig/fnsig.out`.
- Work directory: `libc/` (upstream clone), `crate/`, `core-only/`, `fnsig/`,
  `results/` (the ctest binaries' output).

## Notes

- std is built from source (`-Zbuild-std`), and cargo fingerprints the
  standard library's crates by version, not by file times: after changing the
  fork, `cargo clean` a crate (e.g. `../stdcheck`) before rebuilding it, or
  its std keeps the old libc. The ctest binaries test their own (non-std)
  copy of the fork, which cargo does rebuild.
- The board is shared: `klauss-linux-run` waits for both locks before every ssh
  and scp, so a run can sit idle while another session holds the board.

## Results (2026-11-18, fork at 70ad877)

All pass: ctest 9,944, linux_fcntl 7, linux_if_arp 1, linux_ipv6 5,
linux_elf 52, linux_strerror_r 1, linux_termios 32; fnsig 740 functions;
prims 22 types.
