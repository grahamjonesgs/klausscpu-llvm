#!/bin/bash
# Check the libc crate (the KlaussCPU fork) against KlaussCPU's musl/kernel
# headers. See README.md.
#   ./run.sh            build, host checks, then the ctest binaries on the board
#   NO_BOARD=1 ./run.sh build and host checks only
set -u
cd "$(dirname "$0")"
rc=0
echo "### build (ctest generation, C side compiled with klausscpu clang)"
./build.sh || { echo "BUILD FAILED"; exit 1; }
echo "### host checks: function signatures and C primitive types"
fnsig/run-fnsig.sh || rc=1
if [ -z "${NO_BOARD:-}" ]; then
    echo "### board: ctest binaries"
    rm -rf "${KLAUSSCPU_LIBC_TEST_WORK:-$HOME/.cache/klausscpu-libc-test}/results"
    ./run-board.sh || rc=1
    ./summary.sh
fi
[ $rc -eq 0 ] && echo "ALL CHECKS PASSED" || echo "SOME CHECKS FAILED"
exit $rc
