#!/usr/bin/env bash
# setup-crate-forks.sh <rust checkout>: the crates.io crates that rustc,
# bootstrap and cargo take from forks inside the rust checkout (the
# [patch.crates-io] entries of rustc-patches/0004, 0006 and cargo/0001):
# each is downloaded from crates.io, committed as is, and the KlaussCPU
# patches are applied with git am.  The directories are added to the
# checkout's .git/info/exclude.  Existing directories are left alone (delete
# one to make it again).
#
#   directory                        crate            patches
#   libc-klausscpu                   libc 0.2.185     libc-patches/        (std, rustc)
#   libc-0.2.186-klausscpu           libc 0.2.186     libc-patches/        (cargo)
#   cc-klausscpu                     cc 1.2.28        crate-patches/cc-1.2.28 (bootstrap)
#   linux-raw-sys-klausscpu          linux-raw-sys 0.11.0  crate-patches/... (rustc)
#   linux-raw-sys-0.12.1-klausscpu   linux-raw-sys 0.12.1  crate-patches/... (cargo)
#   rustix-klausscpu                 rustix 1.1.2     crate-patches/...    (rustc)
#   rustix-1.1.4-klausscpu           rustix 1.1.4     crate-patches/...    (cargo)
#   nix-0.26.4-klausscpu             nix 0.26.4       crate-patches/...    (serialport, for
#                                    klausscc built on the board)
#
# Only std needs libc-klausscpu; the rest are for a rustc and cargo that run
# on the board (klausscpu-runtime RUST_ON_BOARD_PLAN.md).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
[ $# = 1 ] || { echo "usage: $0 <rust checkout>" >&2; exit 2; }
RUST=$(cd "$1" && pwd)
[ -d "$RUST/library" ] || { echo "$RUST is not a rust checkout" >&2; exit 2; }
fork() { # fork <dir> <crate> <version> <patch dir>
	local dir=$1 crate=$2 ver=$3 patches=$4 d=$RUST/$1
	if [ -e "$d" ]; then
		echo "$dir: exists, left alone"
		return
	fi
	mkdir "$d"
	curl -sSfL "https://static.crates.io/crates/$crate/$crate-$ver.crate" | tar xz --strip-components=1 -C "$d"
	git -C "$d" init -q
	git -C "$d" add -A
	git -C "$d" commit -qm "$crate $ver from crates.io"
	git -C "$d" am -q "$patches"/*.patch
	grep -qx "/$dir/" "$RUST/.git/info/exclude" 2>/dev/null || echo "/$dir/" >>"$RUST/.git/info/exclude"
	echo "$dir: $crate $ver + $(ls "$patches"/*.patch | wc -l) patches"
}
fork libc-klausscpu libc 0.2.185 "$HERE/libc-patches"
fork libc-0.2.186-klausscpu libc 0.2.186 "$HERE/libc-patches"
fork cc-klausscpu cc 1.2.28 "$HERE/crate-patches/cc-1.2.28"
fork linux-raw-sys-klausscpu linux-raw-sys 0.11.0 "$HERE/crate-patches/linux-raw-sys-0.11.0"
fork linux-raw-sys-0.12.1-klausscpu linux-raw-sys 0.12.1 "$HERE/crate-patches/linux-raw-sys-0.12.1"
fork rustix-klausscpu rustix 1.1.2 "$HERE/crate-patches/rustix-1.1.2"
fork rustix-1.1.4-klausscpu rustix 1.1.4 "$HERE/crate-patches/rustix-1.1.4"
fork nix-0.26.4-klausscpu nix 0.26.4 "$HERE/crate-patches/nix-0.26.4"
