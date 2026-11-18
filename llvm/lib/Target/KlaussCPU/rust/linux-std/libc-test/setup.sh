#!/bin/bash
# Prepare the work directory ($KLAUSSCPU_LIBC_TEST_WORK, see env.sh):
#   libc/        rust-lang/libc 0.2.185 (shallow clone; only its libc-test is used)
#   crate/       libc-test with the KlaussCPU build.rs, Cargo.toml and config
#   core-only/   a `core` for the JSON target (for ctest's macro expansion)
# The clone is fetched once; crate/ and core-only/ sources are rewritten on every
# run, their target/ directories are kept.
set -eu
. "$(dirname "$0")/env.sh"
TAG=0.2.185
if [ ! -d "$WORK/libc/libc-test" ]; then
    echo "### fetching rust-lang/libc $TAG"
    rm -rf "$WORK/libc"
    git clone -q --depth 1 --branch "$TAG" https://github.com/rust-lang/libc "$WORK/libc"
fi
U=$WORK/libc/libc-test
[ -f "$U/build.rs" ] || ltdie "no libc-test in $WORK/libc"
C=$WORK/crate
rm -rf "$C/tests" "$C/src" "$C/build.rs" "$C/.cargo"
mkdir -p "$C/.cargo" "$C/src" "$C/tests"
for t in ctest linux_fcntl linux_if_arp linux_ipv6 linux_elf linux_strerror_r linux_termios; do
    cp "$U/tests/$t.rs" "$C/tests/"
done
cp "$U"/src/*.c "$C/src/"
cp "$U/build.rs" "$C/build.rs"
patch -s -p1 -d "$C" < "$LT/crate/build.rs.patch"
sed "s|@LIBC_SRC@|$LIBC_SRC|" "$LT/crate/Cargo.toml.in" > "$C/Cargo.toml"
cp "$LT/crate/Cargo.lock" "$C/Cargo.lock"
cp "$LT/crate/rustc-expand-wrapper.sh" "$C/"
cat > "$C/.cargo/config.toml" <<EOT
[unstable]
build-std = ["std", "panic_unwind"]
json-target-spec = true

[build]
target = "$SPEC"

[target.klausscpu-unknown-linux-musl]
linker = "$LLVM_BIN/clang"
rustflags = ["-Zunstable-options", "-Clink-arg=--sysroot=$SYSROOT"]

[env]
CC_klausscpu_unknown_linux_musl = "$LLVM_BIN/clang"
CFLAGS_klausscpu_unknown_linux_musl = "--target=klausscpu-unknown-linux-musl --sysroot=$SYSROOT"
AR_klausscpu_unknown_linux_musl = "$LLVM_BIN/llvm-ar"
RUST_LIBC_UNSTABLE_MUSL_V1_2_3 = "1"
EOT
K=$WORK/core-only
mkdir -p "$K/.cargo" "$K/src"
cp "$LT/core-only/Cargo.toml" "$LT/core-only/Cargo.lock" "$K/"
cp "$LT/core-only/src/lib.rs" "$K/src/"
cat > "$K/.cargo/config.toml" <<EOT
[unstable]
build-std = ["core"]
json-target-spec = true

[build]
target = "$SPEC"

[target.klausscpu-unknown-linux-musl]
rustflags = ["-Zunstable-options"]
EOT
