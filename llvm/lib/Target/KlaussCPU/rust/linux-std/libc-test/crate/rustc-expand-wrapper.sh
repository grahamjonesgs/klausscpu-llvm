#!/bin/bash
# ctest expands the libc crate with `$RUSTC -Zunpretty=expanded --target <TARGET>`.
# klausscpu-unknown-linux-musl is a JSON target: rustc only accepts it with
# -Zunstable-options, by path, and there is no installed sysroot for it, so
# `core` and `compiler_builtins` come from the work directory's core-only
# crate (built by build.sh with -Zbuild-std=core for the same JSON file, so
# the target hashes match).
# build.rs points ctest at this wrapper and sets REAL_RUSTC; env.sh exports
# KLAUSSCPU_SPEC (the JSON file) and KLAUSSCPU_LIBC_TEST_WORK (the work dir).
H=${KLAUSSCPU_LIBC_TEST_WORK:?run through run.sh (env.sh sets it)}
JSON=${KLAUSSCPU_SPEC:?run through run.sh (env.sh sets it)}
newest() { ls -t $(find "$H/core-only/target" -name "$1" 2>/dev/null) 2>/dev/null | head -1; }
CORE=$(newest 'libcore-*.rmeta')
CB=$(newest 'libcompiler_builtins-*.rmeta')
if [ -z "$CORE" ] || [ -z "$CB" ]; then
    echo "rustc-expand-wrapper: no core rmetas under $H/core-only/target; run run.sh" >&2
    exit 1
fi
args=()
while [ $# -gt 0 ]; do
    if [ "$1" = "--target" ] && [ "$2" = "klausscpu-unknown-linux-musl" ]; then
        args+=(--target "$JSON"); shift 2
    else
        args+=("$1"); shift
    fi
done
exec "$REAL_RUSTC" -Zunstable-options --extern "core=$CORE" \
    --extern "compiler_builtins=$CB" "${args[@]}"
