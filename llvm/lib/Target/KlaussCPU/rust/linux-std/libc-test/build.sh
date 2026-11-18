#!/bin/bash
# Set up the work directory and build the ctest binaries for
# klausscpu-unknown-linux-musl (see README.md).
set -e
LT=$(cd "$(dirname "$0")" && pwd)
"$LT/setup.sh"
. "$LT/env.sh"
(cd "$WORK/core-only" && cargo $KLAUSSCPU_CARGO_TOOLCHAIN build --release -q)
cd "$WORK/crate"
cargo $KLAUSSCPU_CARGO_TOOLCHAIN build --release "$@" 2>&1 | grep -v '^  \(cargo:\|[A-Za-z_-]* = \)\|Wgcc-install-dir\|found to be present in multiple\|^  \* `\(bin\|integration-test\)` target'
exit ${PIPESTATUS[0]}
