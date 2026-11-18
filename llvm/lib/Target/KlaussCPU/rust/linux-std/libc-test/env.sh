# Sourced by the scripts in this directory: tool locations, the work
# directory, and the environment cargo, build.rs and the wrapper read.
# Same variables as ../../klauss-linux-cargo and ../../klauss-linux-run:
#   KLAUSSCPU_RUSTC          the patched stage1 rustc (required; also read from
#                            ../../.klaussbuild.env)
#   KLAUSSCPU_LLVM_BIN       the fork's build/bin (default <repo>/build/bin)
#   KLAUSSCPU_LINUX_SYSROOT  musl + kernel headers sysroot (default
#                            ~/Documents/src/klausscpu-runtime/linux-ws/dyn/sysroot)
#   KLAUSSCPU_CARGO_TOOLCHAIN  default +nightly if rustup has it
#   KLAUSSCPU_BOARD, KLAUSSCPU_BOARD_KEY  as klauss-linux-run
#   KLAUSSCPU_BOARD_DIR      where the test binaries go on the board
#                            (default /data/libc-test)
# and, for this harness:
#   KLAUSSCPU_LIBC_SRC       the libc fork under test (default
#                            <rust checkout of KLAUSSCPU_RUSTC>/libc-klausscpu)
#   KLAUSSCPU_LIBC_TEST_WORK work directory (default ~/.cache/klausscpu-libc-test)
#   KLAUSSCPU_LIBC_TEST_SKIP_KERNEL_CONSTS=1  upstream's behaviour for kernel constants
LT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
RUSTDIR=$(cd "$LT/../.." && pwd)
REPO=$(cd "$RUSTDIR/../../../../.." && pwd)
[ -f "$RUSTDIR/.klaussbuild.env" ] && . "$RUSTDIR/.klaussbuild.env"
ltdie() { echo "libc-test: $*" >&2; exit 1; }
[ -n "${KLAUSSCPU_RUSTC:-}" ] || ltdie "set KLAUSSCPU_RUSTC to the patched stage1 rustc (../../README.md part 2)"
[ -x "$KLAUSSCPU_RUSTC" ] || ltdie "no rustc at $KLAUSSCPU_RUSTC"
LLVM_BIN=${KLAUSSCPU_LLVM_BIN:-$REPO/build/bin}
SYSROOT=${KLAUSSCPU_LINUX_SYSROOT:-$HOME/Documents/src/klausscpu-runtime/linux-ws/dyn/sysroot}
[ -x "$LLVM_BIN/clang" ] || ltdie "no clang in $LLVM_BIN (set KLAUSSCPU_LLVM_BIN)"
[ -f "$SYSROOT/lib/libc.a" ] || ltdie "no musl sysroot at $SYSROOT (set KLAUSSCPU_LINUX_SYSROOT)"
LIBC_SRC=${KLAUSSCPU_LIBC_SRC:-$(cd "$(dirname "$KLAUSSCPU_RUSTC")/../../../.." && pwd)/libc-klausscpu}
[ -f "$LIBC_SRC/src/lib.rs" ] || ltdie "no libc fork at $LIBC_SRC (set KLAUSSCPU_LIBC_SRC)"
WORK=${KLAUSSCPU_LIBC_TEST_WORK:-$HOME/.cache/klausscpu-libc-test}
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
SPEC=$RUSTDIR/klausscpu-unknown-linux-musl.json
if [ -z "${KLAUSSCPU_CARGO_TOOLCHAIN+set}" ]; then
    if command -v rustup >/dev/null && rustup toolchain list 2>/dev/null | grep -q '^nightly'; then
        KLAUSSCPU_CARGO_TOOLCHAIN=+nightly
    else
        KLAUSSCPU_CARGO_TOOLCHAIN=
    fi
fi
case "$KLAUSSCPU_CARGO_TOOLCHAIN" in
+nightly*) ;;
*) export RUSTC_BOOTSTRAP=1 ;;
esac
export KLAUSSCPU_LIBC_SRC=$LIBC_SRC KLAUSSCPU_LIBC_TEST_WORK=$WORK KLAUSSCPU_SPEC=$SPEC
export RUSTC=$KLAUSSCPU_RUSTC
