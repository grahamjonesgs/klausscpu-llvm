#!/bin/sh
# Build ext_hello.llext from the workspace: staticlib (fat LTO, no
# compiler-builtins-mem) -> drop weak mem* members -> ld.lld -r -u main ->
# ELFCLASS32 ET_REL .llext, then verify the loader invariants.
#
# Same pipeline as ../../../hello-ext/build-llext.sh, but the program is
# built from the klauss-* crates. See ../../README.md.
set -e

BIN=${KLAUSSCPU_LLVM_BIN:-/Users/gjonesblackcyton/Documents/src/llvm-project/build/bin}
RUSTC=${KLAUSSCPU_RUSTC:-/Users/gjonesblackcyton/Documents/src/klausscpu-rust/build/x86_64-apple-darwin/stage1/bin/rustc}
CARGO=${CARGO:-"$HOME/.cargo/bin/cargo"}

# Workspace root (two levels up from this example).
WS="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$WS"

# -Z build-std-features overrides the workspace config's compiler-builtins-mem
# so mem* stay undefined and resolve against the kernel's exports.
RUSTC=$RUSTC "$CARGO" +nightly build -p ext-hello --release \
    -Z build-std-features=optimize_for_size

REL=target/klausscpu-unknown-none-elf/release
OUT="$(dirname "$0")/ext_hello.llext"

# Drop the compiler_builtins members that weakly define mem* — the kernel
# exports them; pulling the in-extension copy drags ~250 KB of float/libm in.
rm -rf "$REL/llext-ar"
mkdir -p "$REL/llext-ar"
(
    cd "$REL/llext-ar"
    "$BIN/llvm-ar" x ../libext_hello.a
    for o in *.o; do
        if "$BIN/llvm-nm" "$o" 2>/dev/null \
             | grep -qE "^[0-9a-f]+ [Ww] (memcpy|memset|memmove|memcmp)$"; then
            rm "$o"
        fi
    done
    "$BIN/llvm-ar" rcs nomem.a ./*.o
)

# -u main seeds archive-member selection (ld -r cannot gc-sections).
"$BIN/ld.lld" -r -m elf32klausscpu -u main -o "$OUT" "$REL/llext-ar/nomem.a"
"$BIN/llvm-strip" --strip-debug --strip-unneeded "$OUT"

echo "==> $(basename "$OUT") built ($(wc -c < "$OUT" | tr -d ' ') bytes)"
"$BIN/llvm-readelf" -h "$OUT" | grep -E "Class:.*ELF32|Type:.*REL|Machine" || {
    echo "FAIL: not an ELF32 ET_REL EM_KLAUSSCPU object"; exit 1; }
BADRELOC=$("$BIN/llvm-readelf" -r "$OUT" | awk '/R_/ {print $3}' | sort -u \
    | grep -vE "R_KLAUSSCPU_(ABS32|ABS64|PCREL32)" || true)
[ -z "$BADRELOC" ] || { echo "FAIL: unsupported relocations:"; echo "$BADRELOC"; exit 1; }
echo "==> relocations OK, imports (must be in ssh/llext_exports.c):"
"$BIN/llvm-nm" -u "$OUT" | sed 's/^/      /'
"$BIN/llvm-nm" "$OUT" | grep -q " T main$" || { echo "FAIL: no 'main'"; exit 1; }
echo "==> 'main' exported. copy to SD and:  run $(basename "$OUT")"
