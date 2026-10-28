#!/usr/bin/env bash
# run_rust_abi_test.sh [FIRST_SEED] [COUNT] [NFUNCS]
#
# Rust <-> C ABI conformance test (C ABI version 2, ../../ABI.md).  For each
# seed, gen_rust_abi_test.py writes a caller and a callee, each in C and in
# Rust (random argument/return type mixes: integers of every width, bool,
# pointers, f32/f64, i128, #[repr(C)] structs and unions of 1-32 bytes,
# variadic functions).  Host gcc runs the C pair for the expected output; then
# a C caller with a Rust callee, a Rust caller with a C callee and Rust with
# Rust are built for KlaussCPU at every combination of the optimisation levels
# (C: $COPTS, default -O0 -O1 -O2 -Os; Rust: $ROPTS, default 0 1 2 s), run on
# the klausscc emulator and compared line by line.
#
#   KLAUSSCPU_RUSTC     the stage1 rustc built against this LLVM (required;
#                       ../README.md section 2)
#   KLAUSSCPU_LLVM_BIN  toolchain bin dir (default <repo>/build/bin)
#   KLAUSSCPU_RUNTIME   klausscpu-runtime checkout, for crt0, the UART stubs and
#                       picolibc-install (default <repo>/../klausscpu-runtime)
#   KLAUSSCC            klausscc (default <runtime>/../klausscc/target/release/klausscc)
#   CARGO               cargo for building core (nightly, or any cargo: the
#                       script sets RUSTC_BOOTSTRAP=1)
#   JOBS                seeds run in parallel (default nproc)
#   OUT                 work directory (default: a temporary one, removed)
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RUSTDIR=$(dirname "$HERE")
REPO=$(cd "$RUSTDIR/../../../../.." && pwd)
LLVM_BIN=${KLAUSSCPU_LLVM_BIN:-$REPO/build/bin}
RT=${KLAUSSCPU_RUNTIME:-$REPO/../klausscpu-runtime}
EMU=${KLAUSSCC:-$RT/../klausscc/target/release/klausscc}
RUSTC=${KLAUSSCPU_RUSTC:?set KLAUSSCPU_RUSTC to the stage1 rustc}
CARGO=${CARGO:-cargo}
SPEC=$RUSTDIR/klausscpu-unknown-none-elf.json
PICO=$RT/picolibc-install
COPTS=${COPTS:-"-O0 -O1 -O2 -Os"}
ROPTS=${ROPTS:-"0 1 2 s"}
JOBS=${JOBS:-$(nproc)}
FIRST=${1:-1}; COUNT=${2:-4}; NFUNCS=${3:-60}
if [ -n "${OUT:-}" ]; then mkdir -p "$OUT"; else OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT; fi
OUT=$(cd "$OUT" && pwd)

# A tool that fails is retried: a toolchain being rebuilt next to the test can
# be missing for a moment.
retry() { local i; for i in 1 2 3; do "$@" && return 0; sleep 15; done; return 1; }
CC="retry $LLVM_BIN/clang -target klausscpu-unknown-elf"
CFLAGS="-isystem $PICO/include -nostdlib -fPIC -D__IEEE_LITTLE_ENDIAN -D_LDBL_EQ_DBL -std=c2x -w"
BUILTINS=$($CC -rtlib=compiler-rt -print-libgcc-file-name)

# The C runtime: crt0, UART stdio.
mkdir -p "$OUT/rt"
for s in crt0 uart_stubs syscalls stdio_handles; do
  $CC $CFLAGS -O1 -I"$RT" -c "$RT/src/$s.c" -o "$OUT/rt/$s.o" || exit 2
done
RTOBJS="$OUT/rt/crt0.o $OUT/rt/uart_stubs.o $OUT/rt/syscalls.o $OUT/rt/stdio_handles.o"

# core and compiler_builtins for the target, once (cargo -Zbuild-std).  Without
# compiler_builtins' mem feature: picolibc provides memcpy and friends.
SR=$OUT/sysroot
mkdir -p "$SR/src"
cat > "$SR/Cargo.toml" <<'EOF'
[package]
name = "abi_sysroot"
version = "0.0.0"
edition = "2021"
[lib]
path = "src/lib.rs"
[profile.release]
panic = "abort"
EOF
echo '#![no_std]' > "$SR/src/lib.rs"
( cd "$SR" && RUSTC="$RUSTC" RUSTC_BOOTSTRAP=1 RUSTFLAGS="-Zunstable-options" \
    "$CARGO" build -q --release --target "$SPEC" -Zbuild-std=core -Zjson-target-spec ) \
  || { echo "building core failed"; exit 2; }
DEPS=$SR/target/klausscpu-unknown-none-elf/release/deps
RLIBS="$(ls "$DEPS"/libcore-*.rlib "$DEPS"/libcompiler_builtins-*.rlib)"
RUSTFLAGS_COMMON="--edition 2021 --target $SPEC -Zunstable-options --crate-type=lib --emit=obj -C panic=abort -C codegen-units=1 -L crate=$DEPS"

# The panic handler (a panic reports test -1 and parks: the run then
# misses ABI-DONE).
cat > "$OUT/rt/panic.rs" <<'EOF'
#![no_std]
extern "C" { fn report(i: i32, h: u64); }
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    unsafe { report(-1, 0xdead) };
    loop {}
}
EOF
"$RUSTC" $RUSTFLAGS_COMMON -C opt-level=1 "$OUT/rt/panic.rs" -o "$OUT/rt/panic.o" || exit 2

run_seed() {
  local seed=$1 W=$OUT/s$1 fail=0 runs=0 O OA OB E got
  mkdir -p "$W"
  python3 -I "$HERE/gen_rust_abi_test.py" "$seed" "$W" "$NFUNCS" || return 2
  gcc -std=c2x -O0 -w "$W/support.c" "$W/caller.c" "$W/callee.c" -o "$W/host" || return 2
  "$W/host" > "$W/expected.txt" || return 2
  $CC $CFLAGS -O1 -c "$W/support.c" -o "$W/support.o" || return 2
  for O in $COPTS; do
    for s in caller callee; do
      $CC $CFLAGS $O -c "$W/$s.c" -o "$W/c-$s$O.o" || { echo "seed $seed: $s.c $O does not compile"; fail=1; }
    done
  done
  for O in $ROPTS; do
    for s in caller callee; do
      "$RUSTC" $RUSTFLAGS_COMMON -C opt-level=$O "$W/$s.rs" -o "$W/rs-$s-O$O.o" 2> "$W/rs-$s-O$O.err" \
        || { echo "seed $seed: $s.rs -O$O does not compile"; head -5 "$W/rs-$s-O$O.err"; fail=1; }
    done
  done
  # caller/callee object pairs: C->Rust, Rust->C, Rust->Rust
  local pairs=()
  for OA in $COPTS; do for OB in $ROPTS; do pairs+=("c-caller$OA.o rs-callee-O$OB.o"); done; done
  for OA in $ROPTS; do for OB in $COPTS; do pairs+=("rs-caller-O$OA.o c-callee$OB.o"); done; done
  for OA in $ROPTS; do for OB in $ROPTS; do pairs+=("rs-caller-O$OA.o rs-callee-O$OB.o"); done; done
  for p in "${pairs[@]}"; do
    set -- $p
    E=$W/t-${1%.o}-${2%.o}.elf
    got=$W/got-${1%.o}-${2%.o}.txt
    [ -f "$W/$1" ] && [ -f "$W/$2" ] || continue
    retry "$LLVM_BIN/ld.lld" -T "$RT/klausscpu.ld" --gc-sections -o "$E" $RTOBJS "$W/support.o" \
      "$W/$1" "$W/$2" "$OUT/rt/panic.o" $RLIBS "$PICO/lib/libc.a" "$BUILTINS" 2> "$E.ldlog" \
      || { echo "seed $seed: $1 + $2 does not link"; grep -v "neither ET_REL" "$E.ldlog" | head -5; fail=1; continue; }
    "$EMU" -i "$E" --emulate --max-instructions 200000000 </dev/null 2>&1 \
      | tr -d '\r' | grep -E '^(T-?[0-9]+ |ABI-DONE)' > "$got"
    runs=$((runs + 1))
    if ! cmp -s "$W/expected.txt" "$got"; then
      echo "seed $seed: $1 + $2 MISMATCH"
      diff "$W/expected.txt" "$got" | head -5
      fail=1
    fi
    rm -f "$E" "$E.ldlog"
  done
  echo "seed $seed: $runs runs $([ $fail = 0 ] && echo ok || echo FAILED)"
  return $fail
}
export -f run_seed retry
export OUT NFUNCS HERE CC CFLAGS COPTS ROPTS RUSTC RUSTFLAGS_COMMON LLVM_BIN RT RTOBJS RLIBS PICO BUILTINS EMU

seq "$FIRST" $((FIRST + COUNT - 1)) | xargs -P "$JOBS" -I{} bash -c 'run_seed {}' > "$OUT/log.txt" 2>&1
cat "$OUT/log.txt"
ok=$(grep -c ': [0-9]* runs ok$' "$OUT/log.txt")
runs=$(grep -o ': [0-9]* runs' "$OUT/log.txt" | awk '{s += $2} END {print s + 0}')
echo "rust abi-test: seeds $FIRST..$((FIRST + COUNT - 1)), $NFUNCS functions each, $runs runs, $ok/$COUNT seeds ok"
[ "$ok" = "$COUNT" ]
