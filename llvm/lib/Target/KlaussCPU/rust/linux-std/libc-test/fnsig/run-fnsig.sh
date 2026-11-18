#!/bin/bash
# Host-only checks (no board): function signatures (fnsig.py) and the C
# primitive types (prims.py). Needs the crate built first (build.sh: for
# ctest_output.c and core-only). Everything generated goes in $WORK/fnsig.
set -eu
cd "$(dirname "$0")"
FS=$PWD
. ../env.sh
export REAL_RUSTC=$KLAUSSCPU_RUSTC
W=$WORK/fnsig
mkdir -p "$W/work"
LIBC=$LIBC_SRC/src/lib.rs
WRAP=$WORK/crate/rustc-expand-wrapper.sh
OUT=$(ls -td "$WORK"/crate/target/klausscpu-unknown-linux-musl/release/build/libc-test-klausscpu/*/out | while read d; do [ -f "$d/ctest_output.c" ] && echo "$d" && break; done)
[ -n "$OUT" ] || { echo "run-fnsig: build the crate first (build.sh)" >&2; exit 1; }
# The same expansion ctest makes (same cfgs as the libc build script emits).
RUSTC_BOOTSTRAP=1 "$WRAP" -Zunpretty=expanded --edition 2021 \
    --cfg musl_v1_2_3 --target klausscpu-unknown-linux-musl "$LIBC" > "$W/expanded.rs" 2> "$W/expand.err" \
    || { cat "$W/expand.err"; exit 1; }
rc=0
python3 -I "$FS/fnsig.py" "$W/expanded.rs" "$OUT/ctest_output.c" "$W/work" "$LLVM_BIN/clang++" "$SYSROOT" > "$W/fnsig.out" || rc=1
grep -v '^NOTE' "$W/fnsig.out" || true
echo "  ($(grep -c '^NOTE' "$W/fnsig.out") NOTE lines in $W/fnsig.out: 8-byte integer vs pointer)"
RUSTC_BOOTSTRAP=1 python3 -I "$FS/prims.py" "$W/work" "$LLVM_BIN/clang" "$SYSROOT" "$WRAP" || rc=1
exit $rc
