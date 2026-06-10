#!/usr/bin/env bash
#
# build-builtins.sh — build compiler-rt's builtins (soft-FP + integer divide)
# for the KlaussCPU bare-metal target and install the archive into the clang
# resource dir, where the driver auto-links `-lclang_rt.builtins`.
#
# Why this exists: the Zephyr build links via the clang driver, which adds
# `-lclang_rt.builtins` and looks for libclang_rt.builtins.a in clang's resource
# dir.  That archive is a *build artifact* (lives under build/, not committed),
# so it is lost whenever build/ is wiped or the toolchain is rebuilt — re-run
# this script then.  (The bare-metal Makefile links individual crt-*.o instead
# and doesn't need this.)
#
# Usage:  ./build-builtins.sh         # uses $KLAUSSCPU_LLVM_BIN or build/bin
#
set -euo pipefail

ROOT="$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
LLVM_BIN="${KLAUSSCPU_LLVM_BIN:-$ROOT/build/bin}"
CLANG="$LLVM_BIN/clang"
AR="$LLVM_BIN/llvm-ar"
B="$ROOT/compiler-rt/lib/builtins"
TRIPLE="klausscpu-unknown-elf"

[ -x "$CLANG" ] || { echo "clang not found at $CLANG — build the LLVM toolchain first"; exit 1; }

# Ask the driver exactly where it expects the archive (handles clang-version and
# triple normalisation, e.g. klausscpu-unknown-unknown-elf).
DEST="$("$CLANG" --target="$TRIPLE" -rtlib=compiler-rt -print-libgcc-file-name)"
mkdir -p "$(dirname "$DEST")"

# Same flags the runtime Makefile uses for crt-*.o, plus -fPIC (the Zephyr image
# is built -fPIC, so the builtins must be too).
FLAGS=(--target="$TRIPLE" -O1 -fPIC -nostdlibinc -I"$B" -D__SOFTFP__)

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Compile compiler-rt's portable GENERIC_SOURCES set.  The linker pulls only the
# objects it needs from the archive, so compiling the whole portable set is fine;
# files that don't apply to KlaussCPU simply fail to compile and are skipped.
ok=0; skip=0
while read -r f; do
	[ -f "$B/$f" ] || continue
	if "$CLANG" "${FLAGS[@]}" -c "$B/$f" -o "$TMP/${f%.c}.o" 2>/dev/null; then
		ok=$((ok + 1))
	else
		skip=$((skip + 1))
	fi
done < <(awk '/set\(GENERIC_SOURCES/,/^[[:space:]]*\)/' "$B/CMakeLists.txt" \
		| grep -oE '[A-Za-z0-9_]+\.c')

"$AR" rcs "$DEST" "$TMP"/*.o
echo "built  $DEST"
echo "  compiled=$ok skipped=$skip members=$("$AR" t "$DEST" | wc -l | tr -d ' ')"
