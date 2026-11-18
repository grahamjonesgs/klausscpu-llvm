#!/bin/bash
# Run the ctest binaries on the board (klauss-linux-run copies each one to
# $KLAUSSCPU_BOARD_DIR, default /data/libc-test, and runs it there; it holds
# both board locks) and collect the output in $WORK/results/.
set -u
. "$(dirname "$0")/env.sh"
export KLAUSSCPU_BOARD_DIR=${KLAUSSCPU_BOARD_DIR:-/data/libc-test}
BIN=$WORK/crate/target/klausscpu-unknown-linux-musl/release
TESTS="${TESTS:-ctest linux_fcntl linux_if_arp linux_ipv6 linux_elf linux_strerror_r linux_termios}"
mkdir -p "$WORK/results"
rc=0
for t in $TESTS; do
    echo "== $t"
    # The binaries exit non-zero (panic) when a check fails.
    "$RUSTDIR/klauss-linux-run" "$BIN/$t" > "$WORK/results/$t.out" 2>&1
    r=$?
    echo "exit $r" >> "$WORK/results/$t.out"
    [ $r -eq 0 ] || rc=1
    grep -c '^bad ' "$WORK/results/$t.out" | sed 's/^/  failed checks: /'
    tail -3 "$WORK/results/$t.out" | sed 's/^/  /'
done
exit $rc
