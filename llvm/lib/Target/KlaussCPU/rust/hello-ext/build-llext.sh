#!/bin/sh
# Build hello_rs.llext: staticlib (fat LTO) -> ld.lld -r -> ET_REL .llext,
# then verify every invariant the Zephyr LLEXT loader relies on.
set -e

BIN=${KLAUSSCPU_LLVM_BIN:-/Users/gjonesblackcyton/Documents/src/llvm-project/build/bin}
RUSTC=${KLAUSSCPU_RUSTC:-/Users/gjonesblackcyton/Documents/src/klausscpu-rust/build/x86_64-apple-darwin/stage1/bin/rustc}
CARGO=${CARGO:-"$HOME/.cargo/bin/cargo"}

cd "$(dirname "$0")"
RUSTC=$RUSTC "$CARGO" +nightly build --release

REL=target/klausscpu-unknown-none-elf/release

# Drop the compiler_builtins members that weakly define mem* — the kernel
# exports memcpy/memset/memcmp/memmove, and pulling the in-extension copy
# drags a ~250 KB transitive web of float/libm CGUs into the image.
rm -rf "$REL/llext-ar"
mkdir -p "$REL/llext-ar"
(
    cd "$REL/llext-ar"
    "$BIN/llvm-ar" x ../libhello_ext.a
    for o in *.o; do
        if "$BIN/llvm-nm" "$o" 2>/dev/null \
             | grep -qE "^[0-9a-f]+ [Ww] (memcpy|memset|memmove|memcmp)$"; then
            rm "$o"
        fi
    done
    "$BIN/llvm-ar" rcs nomem.a ./*.o
)

# -u main seeds archive-member selection: only the LTO'd crate object and
# the compiler_builtins members it actually references are pulled in
# (ld -r cannot gc-sections, so member granularity is the size control).
"$BIN/ld.lld" -r -m elf32klausscpu -u main -o hello_rs.llext "$REL/llext-ar/nomem.a"
"$BIN/llvm-strip" --strip-debug --strip-unneeded hello_rs.llext

echo "==> hello_rs.llext built ($(wc -c < hello_rs.llext | tr -d ' ') bytes)"

# ── Loader-invariant checks ──────────────────────────────────────────────
"$BIN/llvm-readelf" -h hello_rs.llext | grep -E "Class:.*ELF32|Type:.*REL|Machine" || {
    echo "FAIL: not an ELF32 ET_REL EM_KLAUSSCPU object"; exit 1; }

BADRELOC=$("$BIN/llvm-readelf" -r hello_rs.llext | awk '/R_/ {print $3}' | sort -u \
    | grep -vE "R_KLAUSSCPU_(ABS32|ABS64|PCREL32)" || true)
if [ -n "$BADRELOC" ]; then
    echo "FAIL: relocation types the loader does not implement:"; echo "$BADRELOC"; exit 1
fi
echo "==> relocations OK: $("$BIN/llvm-readelf" -r hello_rs.llext | grep -c R_KLAUSSCPU || true) entries, all ABS32/ABS64/PCREL32"

echo "==> imports (must all be in ssh/llext_exports.c):"
"$BIN/llvm-nm" -u hello_rs.llext | sed 's/^/      /'

"$BIN/llvm-nm" hello_rs.llext | grep -q " T main$" || { echo "FAIL: no 'main' symbol"; exit 1; }
echo "==> 'main' exported, symtab present"
echo "==> copy to SD and run over SSH:  run hello_rs.llext"
